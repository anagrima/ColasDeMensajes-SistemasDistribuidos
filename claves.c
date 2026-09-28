// Parte A: implementación local (no distribuida)
// Diseño con lista enlazada para almacenar los datos, protegido por mutex

#include "claves.h" // Prototipos + struct Paquete
#include <pthread.h>   // mutex
#include <stdlib.h>    // malloc, free
#include <string.h>    // strncpy, strlen, strnlen

#define MAX_STR 256    // 255 chars útiles + '\0'
#define MAX_V2 32      // máximo número de elementos en V_value2 (N_value2 ∈ [1..32])

// Nodo de la lista enlazada para almacenar cada clave y sus valores asociados
typedef struct Nodo {
    char key[MAX_STR];       // Clave (string de hasta 255 chars útiles)
    char value1[MAX_STR];    // Valor1 (string de hasta 255 chars útiles)
    int  N_value2;           // Número de elementos en V_value2
    float V_value2[MAX_V2];  // Vector de valores float
    struct Paquete value3;   // Estructura Paquete
    struct Nodo *next;       // Puntero al siguiente nodo
} Nodo;

// Estado global: lista enlazada de nodos y mutex para protegerla
static Nodo *g_head = NULL; // Puntero al inicio de la lista enlazada
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex para proteger el acceso a la lista enlazada

// Copia segura: trunca a 255 y garantiza terminación en '\0'
static void safe_strcpy_255(char dst[MAX_STR], const char *src) {
    if (!src) { // Si src es NULL -> cadena vacía
        dst[0] = '\0';
        return;
    }
    // Copiamos como máximo 255 y cerramos con '\0'
    strncpy(dst, src, MAX_STR - 1);
    dst[MAX_STR - 1] = '\0';
}

// Buscamos un nodo por key (asume  que el caller garantiza que key no es NULL) -> devuelve puntero o NULL
static Nodo* find_node(const char *key) {
    for (Nodo *cur = g_head; cur != NULL; cur = cur->next) {
        if (strncmp(cur->key, key, MAX_STR) == 0) { // Si coincide la clave (comparación segura)
            return cur;
        }
    }
    return NULL;
}

// Valida longitudes máximas de strings (no nulos, no vacíos, no demasiado largos)
static int validate_strings(const char *key, const char *value1) {
    if (!key || !value1) return 0;                          // Si alguno es NULL -> error
    if (strnlen(key, MAX_STR) > (MAX_STR - 1)) return 0;    // Si key es demasiado larga (más de 255 chars útiles) -> error
    if (strnlen(value1, MAX_STR) > (MAX_STR - 1)) return 0; // Si value1 es demasiado largo (más de 255 chars útiles) -> error
    return 1; // Todo válido
}

// Libera toda la memoria y deja el estado limpio
int destroy(void) { 
    if (pthread_mutex_lock(&g_mutex) != 0) return -1; // Si no se pudo bloquear el mutex -> error
    Nodo *cur = g_head;
    while (cur) {
        Nodo *next = cur->next;
        free(cur);
        cur = next;
    }
    g_head = NULL; // Reiniciamos el puntero al inicio

    // Si algo va mal aquí, señalamos el error explícitamente
    if (pthread_mutex_unlock(&g_mutex) != 0) return -1; 
    return 0; 
}

// Inserta una nueva tupla asociada a la clave si no existe ya de antes
int set_value(char *key, char *value1, int N_value2, float *V_value2, struct Paquete value3) {
    if (!validate_strings(key, value1)) return -1;    // Si los strings no son válidos (NULL o demasiado largos) -> error
    if (N_value2 < 1 || N_value2 > MAX_V2) return -1; // Si N_value2 no está en el rango válido -> error
    if (!V_value2) return -1;                         // Si V_value2 es NULL -> error
    if (pthread_mutex_lock(&g_mutex) != 0) return -1; // Si no se pudo bloquear el mutex -> error

    // Error si ya existe la clave
    if (find_node(key) != NULL) {
        pthread_mutex_unlock(&g_mutex); // Desbloqueamos antes de retornar
        return -1; // Ya existe la clave -> error
    }

    // Creamos un nuevo nodo para la nueva tupla
    Nodo *n = (Nodo*)malloc(sizeof(Nodo)); 
    if (!n) {
        pthread_mutex_unlock(&g_mutex); // Desbloqueamos antes de retornar
        return -1; // Error de memoria
    }

    safe_strcpy_255(n->key, key);   
    safe_strcpy_255(n->value1, value1); 
    n->N_value2 = N_value2; 
    for (int i = 0; i < N_value2; i++) { 
        n->V_value2[i] = V_value2[i];
    }
    // Limpiamos el resto de V_value2 para evitar basura en campos no usados
    for (int i = N_value2; i < MAX_V2; i++) {
        n->V_value2[i] = 0.0f;
    }
    n->value3 = value3; 

    // Insertar al inicio para simplicidad (no requiere recorrer la lista)
    n->next = g_head;
    g_head = n;

    /* No usamos if para comprobar pthread_mutex_unlock porque la operación ya se ha hecho con éxito
    Devolver -1 aquí podría indicar error aunque la clave ya haya sido insertada */
    pthread_mutex_unlock(&g_mutex); // Desbloqueamos el mutex
    return 0; // Éxito
}

// Recupera los valores asociados a una clave existente
int get_value(char *key, char *value1, int *N_value2, float *V_value2, struct Paquete *value3) {
    if (!key || !value1 || !N_value2 || !V_value2 || !value3) return -1; // Si alguno de los punteros es NULL -> error
    // Validamos el formato de la clave (no nula, no vacía, no demasiado larga)
    if (strnlen(key, MAX_STR) > (MAX_STR - 1)) return -1; // Si key no termina en '\0' o es demasiado larga -> error

    if (pthread_mutex_lock(&g_mutex) != 0) return -1;     // Si no se pudo bloquear el mutex -> error
    Nodo *n = find_node(key);
    if (!n) {
        pthread_mutex_unlock(&g_mutex); 
        return -1; 
    }

    safe_strcpy_255(value1, n->value1);  // Copiamos value1 (asume buffer >=256)
    *N_value2 = n->N_value2; 
    for (int i = 0; i < n->N_value2; i++) {
        V_value2[i] = n->V_value2[i];    // Copiamos los valores de V_value2 (asume buffer >=32)
    }
    for (int i = n->N_value2; i < MAX_V2; i++) {
        V_value2[i] = 0.0f;              // Limpiamos el resto de V_value2
    }
    *value3 = n->value3; 
    pthread_mutex_unlock(&g_mutex);
    return 0; 
}

// Modifica los valores asociados a una clave existente
int modify_value(char *key, char *value1, int N_value2, float *V_value2, struct Paquete value3) {
    if (!validate_strings(key, value1)) return -1;     // Si los strings no son válidos (NULL o demasiado largos) -> error
    if (N_value2 < 1 || N_value2 > MAX_V2) return -1;  // Si N_value2 no está en el rango válido -> error
    if (!V_value2) return -1;                          // Si V_value2 es NULL -> error
    if (pthread_mutex_lock(&g_mutex) != 0) return -1;  // Si no se pudo bloquear el mutex -> error
    // Buscamos el nodo existente para modificarlo
    Nodo *n = find_node(key);
    if (!n) {
        pthread_mutex_unlock(&g_mutex); 
        return -1; 
    }

    safe_strcpy_255(n->value1, value1); 
    n->N_value2 = N_value2; 
    for (int i = 0; i < N_value2; i++) {
        n->V_value2[i] = V_value2[i]; 
    }
    for (int i = N_value2; i < MAX_V2; i++) {
        n->V_value2[i] = 0.0f; 
    }
    n->value3 = value3; 
    pthread_mutex_unlock(&g_mutex); 
    return 0; 
}

// Elimina una clave de la lista
int delete_key(char *key) {
    if (!key) return -1; // Si key es NULL -> error
    if (strnlen(key, MAX_STR) > (MAX_STR - 1)) return -1; // Si key no termina en '\0' o es demasiado larga -> error
    if (pthread_mutex_lock(&g_mutex) != 0) return -1; 

    Nodo *cur = g_head; // Puntero para recorrer la lista
    Nodo *prev = NULL;  // Puntero para mantener el nodo anterior (necesario para eliminar)

    // Recorremos la lista para encontrar el nodo con la clave dada
    while (cur) {
        if (strncmp(cur->key, key, MAX_STR) == 0) { // Si encontramos la clave -> la eliminamos
            if (prev) prev->next = cur->next;       // Si hay un nodo anterior -> actualizamos su puntero
            else g_head = cur->next;                // Si no hay nodo anterior -> estamos eliminando el primer nodo, actualizamos g_head

            free(cur);                              
            pthread_mutex_unlock(&g_mutex);         
            return 0; 
        }
        prev = cur; // Actualizamos el nodo anterior antes de avanzar y avanzamos al siguiente nodo
        cur = cur->next; 
    }

    // No usamos if para comprobar pthread_mutex_unlock: el resultado funcional ya es "clave no existe" -1
    pthread_mutex_unlock(&g_mutex); 
    return -1; // No existe la clave
}

// Verifica si una clave existe en la lista: 1 si existe, 0 si no existe
int exist(char *key) {
    if (!key) return -1;                                    // Si key es NULL -> error
    if (strnlen(key, MAX_STR) > (MAX_STR - 1)) return -1;   // Si key no termina en '\0' o es demasiado larga -> error
    if (pthread_mutex_lock(&g_mutex) != 0) return -1;       // Si no se pudo bloquear el mutex -> error

    int res = (find_node(key) != NULL) ? 1 : 0; 
    // No usamos if para comprobar pthread_mutex_unlock: ya hemos calculado el resultado funcional res
    pthread_mutex_unlock(&g_mutex); 
    return res; 
}