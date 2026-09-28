/* Parte B: implementamos la misma API de claves.h, pero en vez de acceder a memoria local,
se empaqueta la operación en un mensaje y la envía a un servidor a través de colas POSIX (mqueue)*/

#define _POSIX_C_SOURCE 200809L // Para exponer APIs POSIX

#include "claves.h"  // prototipos de la API + struct Paquete
#include <mqueue.h>  // colas POSIX
#include <pthread.h> // threads
#include <errno.h>   // errno
#include <stdio.h>   // printf, perror
#include <string.h>  // memset, strncpy, strlen
#include <stdlib.h>  // malloc, free
#include <unistd.h>  // getpid
#include <time.h>    // clock_gettime

#define SRV_QUEUE   "/mq_claves_srv_100495785" // nombre de la cola del servidor
#define MAX_STR 256  // 255 chars útiles + '\0'
#define MAX_V2 32    // máximo número de elementos en V_value2
#define MAX_QNAME 64 // máximo nombre de cola
#define REPLY_TIMEOUT_SEC 2  // timeout para esperar respuesta del servidor (segundos)

// Tipo de operación a ejecutar (parte A)
typedef enum {
    OP_DESTROY = 1,
    OP_SET,
    OP_GET,
    OP_MODIFY,
    OP_DELETE,
    OP_EXIST
} op_t;

// Request que el cliente envía al servidor
typedef struct {
    op_t op;                     // Operación que quiere ejecutar
    char reply_queue[MAX_QNAME]; // Nombre de la cola donde el servidor debe enviar la respuesta

    char key[MAX_STR];       // Clave
    char value1[MAX_STR];    // Valor 1
    int  N_value2;           // Número de elementos en V_value2
    float V_value2[MAX_V2];  // Valores 2
    struct Paquete value3;   // Valor 3
} request_t;

// Response que el servidor envía al cliente
typedef struct {
    int ret;                 // Retorno de la operación del servidor
    char value1[MAX_STR];    
    int  N_value2;           // Para GET
    float V_value2[MAX_V2]; 
    struct Paquete value3;
} response_t;

static pthread_mutex_t g_name_mtx = PTHREAD_MUTEX_INITIALIZER; // Mutex para proteger la generación de nombres únicos de colas de respuesta
static unsigned long g_req_id = 0; // Contador para generar IDs únicos para las colas de respuesta

// Validación de formato de clave: no nula, no vacía, no demasiado larga
static int validate_key(const char *key) {
    if (!key) return 0; // Cadena nula
    if (!key || key[0] == '\0') return 0; // Cadena vacía
    if (strnlen(key, MAX_STR) > (MAX_STR - 1)) return 0; // Cadena demasiado larga
    return 1;
}

// Validación de formato de cadenas (key y value1): no nulas, no vacías, no demasiado largas
static int validate_strings(const char *key, const char *value1) {
    if (!key || !value1) return 0; // Cadena nula
    if (strnlen(key, MAX_STR) > (MAX_STR - 1)) return 0; // Cadena key demasiado larga
    if (strnlen(value1, MAX_STR) > (MAX_STR - 1)) return 0; // Cadena value1 demasiado larga
    return 1;
}

// Función para generar un nombre único para la cola de respuesta del cliente
static int make_reply_queue_name(char out[MAX_QNAME]) {
    pid_t pid = getpid();
    unsigned long id;

    if (pthread_mutex_lock(&g_name_mtx) != 0) return 0; // Si falla el mutex, es un error interno en el cliente (lo tratamos como error de comunicaciones)
    id = ++g_req_id; // Incrementamos el contador para generar un ID único
    pthread_mutex_unlock(&g_name_mtx); // Desbloqueamos el mutex

    int n = snprintf(out, MAX_QNAME, "/mq_claves_cli_%ld_%lu", (long)pid, id); // Generamos el nombre de la cola de respuesta
    return (n > 0 && n < MAX_QNAME); // Si el nombre generado cabe en el buffer -> éxito, si no -> error de comunicaciones
}

// Función para enviar una solicitud al servidor y esperar la respuesta con timeout
static int send_request_and_wait(const request_t *req_in, response_t *resp_out) {
    request_t req = *req_in; // Copia local

    // Crear cola de respuesta
    struct mq_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.mq_maxmsg  = 10;
    attr.mq_msgsize = sizeof(response_t);

    mq_unlink(req.reply_queue); // Aseguramos que no exista una cola con el mismo nombre antes de crearla

    mqd_t reply_mq = mq_open(req.reply_queue, O_CREAT | O_RDONLY, 0600, &attr);
    if (reply_mq == (mqd_t)-1) { // Si no se pudo crear la cola de respuesta -> error de comunicaciones
        return -2;
    }

    // Abrir cola del servidor
    mqd_t srv = mq_open(SRV_QUEUE, O_WRONLY);
    if (srv == (mqd_t)-1) { // Si no se pudo abrir la cola del servidor -> error de comunicaciones
        mq_close(reply_mq);
        mq_unlink(req.reply_queue);
        return -2;
    }

    // Enviar request
    if (mq_send(srv, (const char*)&req, sizeof(req), 0) == -1) { // Si hubo un error al enviar la solicitud -> error de comunicaciones
        mq_close(srv);
        mq_close(reply_mq);
        mq_unlink(req.reply_queue);
        return -2;
    }
    mq_close(srv); // Cerramos la cola del servidor después de enviar la solicitud

    // Esperar respuesta con timeout
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == -1) { // Si hubo un error al obtener el tiempo actual -> error de comunicaciones
        mq_close(reply_mq);
        mq_unlink(req.reply_queue);
        return -2;
    }
    ts.tv_sec += REPLY_TIMEOUT_SEC; // Establecemos el timeout para la recepción de la respuesta

    ssize_t n = mq_timedreceive(reply_mq, (char*)resp_out, sizeof(*resp_out), NULL, &ts); // Intentamos recibir la respuesta del servidor con timeout
    mq_close(reply_mq); // Cerramos la cola de respuesta después de recibir la respuesta
    mq_unlink(req.reply_queue); // Eliminamos la cola de respuesta

    if (n < 0) { // Si hubo un error al recibir (timeout o error real) -> devolvemos -2
        return -2;
    }
    if ((size_t)n != sizeof(*resp_out)) { // Si el tamaño recibido no coincide con el esperado -> devolvemos -2
        return -2;
    }

    return 0; // Comunicación exitosa -> respuesta válida en resp_out
}

// Implementación API pública (claves.h)

int destroy(void) { 
    request_t req; // Solo empaquetamos op y esperamos ret.
    memset(&req, 0, sizeof(req)); 
    req.op = OP_DESTROY;

    if (!make_reply_queue_name(req.reply_queue)) return -2;

    response_t resp; 
    int comm = send_request_and_wait(&req, &resp); 
    if (comm == -2) return -2; 

    return resp.ret; // 0 / -1
}

// Validamos lo que es realmente comprobable antes de mandar la solicitud al servidor para evitar tráfico innecesario
int set_value(char *key, char *value1, int N_value2, float *V_value2, struct Paquete value3) {
    // Validaciones "sin contactar"
    if (!validate_strings(key, value1)) return -1; // Si el formato de las cadenas no es válido -> error "normal" del servicio
    if (N_value2 < 1 || N_value2 > MAX_V2) return -1; // Si el tamaño del array no es válido -> error "normal" del servicio
    if (!V_value2) return -1; // Si el puntero al array es nulo -> error "normal" del servicio

    request_t req; 
    memset(&req, 0, sizeof(req)); 
    req.op = OP_SET; 

    if (!make_reply_queue_name(req.reply_queue)) return -2; // Si no se pudo generar un nombre de cola de respuesta -> error de comunicaciones

    // Copiamos a buffers de tamaño fijo del protocolo (evitamos punteros crudos en mensajes)
    strncpy(req.key, key, MAX_STR - 1); // Copiar clave a la solicitud
    req.key[MAX_STR - 1] = '\0';        // Asegurar terminación de cadena

    strncpy(req.value1, value1, MAX_STR - 1); 
    req.value1[MAX_STR - 1] = '\0';          

    req.N_value2 = N_value2; 

    // Inicializamos el vector completo para que el mensaje sea determinista (sin basura)
    for (int i = 0; i < MAX_V2; i++) req.V_value2[i] = 0.0f; 
    for (int i = 0; i < N_value2; i++) req.V_value2[i] = V_value2[i]; 
    req.value3 = value3;

    response_t resp;
    int comm = send_request_and_wait(&req, &resp); 
    if (comm == -2) return -2; 

    return resp.ret; 
}

// Si la comunicación va bien pero el servidor devuelve -1 (clave no existe), propagamos -1 tal cual
int get_value(char *key, char *value1, int *N_value2, float *V_value2, struct Paquete *value3) {
    // Validación "sin contactar"

    if (!key || !value1 || !N_value2 || !V_value2 || !value3) return -1; // Si algún puntero es nulo -> error "normal" del servicio
    if (!validate_key(key)) return -1; // Si el formato de la clave no es válido -> error "normal" del servicio

    request_t req;                 
    memset(&req, 0, sizeof(req)); 
    req.op = OP_GET;              

    if (!make_reply_queue_name(req.reply_queue)) return -2; 

    strncpy(req.key, key, MAX_STR - 1); // Copiar clave a la solicitud
    req.key[MAX_STR - 1] = '\0';        // Asegurar terminación de cadena

    response_t resp; 
    int comm = send_request_and_wait(&req, &resp); 
    if (comm == -2) return -2; 

    if (resp.ret == 0) {                           // Si la operación fue exitosa -> copiamos los resultados a los punteros de salida
        strncpy(value1, resp.value1, MAX_STR - 1); // Copiar value1 de la respuesta al puntero de salida
        value1[MAX_STR - 1] = '\0';                // Asegurar terminación de cadena

        *N_value2 = resp.N_value2; 
        for (int i = 0; i < MAX_V2; i++) V_value2[i] = resp.V_value2[i]; 
        *value3 = resp.value3; 
    }

    return resp.ret; // 0 / -1
}

// Misma filosofía que set_value, pero la operación exige que la clave exista
int modify_value(char *key, char *value1, int N_value2, float *V_value2, struct Paquete value3) {
    // Validaciones "sin contactar"

    if (!validate_strings(key, value1)) return -1;    // Si el formato de las cadenas no es válido -> error "normal" del servicio
    if (N_value2 < 1 || N_value2 > MAX_V2) return -1; // Si el tamaño del array no es válido -> error "normal" del servicio
    if (!V_value2) return -1;                         // Si el puntero al array es nulo -> error "normal" del servicio

    request_t req; 
    memset(&req, 0, sizeof(req)); 
    req.op = OP_MODIFY; 

    if (!make_reply_queue_name(req.reply_queue)) return -2;

    strncpy(req.key, key, MAX_STR - 1); 
    req.key[MAX_STR - 1] = '\0'; 

    strncpy(req.value1, value1, MAX_STR - 1); 
    req.value1[MAX_STR - 1] = '\0'; 

    req.N_value2 = N_value2; 
    for (int i = 0; i < MAX_V2; i++) req.V_value2[i] = 0.0f;
    for (int i = 0; i < N_value2; i++) req.V_value2[i] = V_value2[i]; 
    req.value3 = value3; 

    response_t resp; 
    int comm = send_request_and_wait(&req, &resp); 
    if (comm == -2) return -2; 

    return resp.ret; // 0 / -1
}

// Operación simple, pero mantiene la distinción -1 vs -2
int delete_key(char *key) {
    // Validación "sin contactar"

    if (!validate_key(key)) return -1; // Si el formato de la clave no es válido -> error "normal" del servicio

    request_t req; // Solicitud
    memset(&req, 0, sizeof(req)); // Inicializar a cero para evitar basura en campos no usados
    req.op = OP_DELETE; 

    if (!make_reply_queue_name(req.reply_queue)) return -2; // Si no se pudo generar un nombre de cola de respuesta -> error de comunicaciones

    strncpy(req.key, key, MAX_STR - 1); // Copiar clave a la solicitud
    req.key[MAX_STR - 1] = '\0';        // Asegurar terminación de cadena

    response_t resp;                               // Variable para recibir la respuesta del servidor
    int comm = send_request_and_wait(&req, &resp); // Enviar solicitud y esperar respuesta
    if (comm == -2) return -2;                     // Si hubo un error de comunicaciones (timeout, error al enviar o recibir) -> devolvemos -2

    return resp.ret; // 0 / -1
}

// Devuelve 1/0/-1 (servicio) o -2 si no hay comunicación
int exist(char *key) {
    // En la versión distribuida: errores del servicio -> -1; errores de comunicaciones -> -2

    if (!validate_key(key)) return -1; // Si el formato de la clave no es válido -> error "normal" del servicio

    request_t req; 
    memset(&req, 0, sizeof(req)); 
    req.op = OP_EXIST; 

    if (!make_reply_queue_name(req.reply_queue)) return -2; 

    strncpy(req.key, key, MAX_STR - 1); 
    req.key[MAX_STR - 1] = '\0'; 

    response_t resp; 
    int comm = send_request_and_wait(&req, &resp); // Enviar solicitud y esperar respuesta
    if (comm == -2) return -2;                     // Si hubo un error de comunicaciones (timeout, error al enviar o recibir) -> devolvemos -2

    return resp.ret; // 1 / 0 / -1
}