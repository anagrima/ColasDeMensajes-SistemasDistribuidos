# Variables del compilador
CC = gcc

# Banderas de compilación:
# -Wall -Wextra: muestran todos los warnings (buena práctica)
# -g: añade información para depurar (Valgrind/GDB)
# -pthread: necesario para compilar hilos y mutex
CFLAGS = -Wall -Wextra -g -pthread

# -fPIC: Position Independent Code (obligatorio para bibliotecas dinámicas .so)
PICFLAGS = -fPIC

# -shared: indica que queremos generar una biblioteca dinámica (.so)
LDFLAGS_SO = -shared

# -Wl,-rpath,'$$ORIGIN': hace que el ejecutable busque la .so en su propio directorio
RPATH = -Wl,-rpath,'$$ORIGIN'


# Regla principal que se ejecuta al escribir simplemente 'make'
all: libclaves.so cliente_local all_b


# 1. Compilar el objeto con código independiente de posición (necesario para .so)
claves.o: claves.c claves.h
	$(CC) $(CFLAGS) $(PICFLAGS) -c claves.c -o claves.o


# 2. Crear la biblioteca dinámica libclaves.so (Parte A)
libclaves.so: claves.o
	$(CC) $(LDFLAGS_SO) -o libclaves.so claves.o -lpthread


# 3. Regla para compilar el cliente local y enlazarlo con libclaves.so
# -L. indica que busque bibliotecas en el directorio actual
# -lclaves enlaza con libclaves.so
# $(RPATH) incrusta la ruta del propio ejecutable para encontrar la .so al ejecutar
cliente_local: app-cliente.c libclaves.so claves.h
	$(CC) $(CFLAGS) -o cliente_local app-cliente.c -L. -lclaves $(RPATH)



# Añadidos para la parte B de la práctica

# Regla "conveniente" para compilar todo (Parte A + Parte B)
# Parte B: construimos un proxy (cliente) como .so + servidor_mq 
all_b: libclaves.so cliente_local libproxyclaves.so cliente_mq servidor_mq

# 4. Compilar el proxy (stub cliente) como biblioteca dinámica libproxyclaves.so (Parte B)
proxy-mq.o: proxy-mq.c claves.h
	$(CC) $(CFLAGS) $(PICFLAGS) -c proxy-mq.c -o proxy-mq.o

libproxyclaves.so: proxy-mq.o
	$(CC) $(LDFLAGS_SO) -o libproxyclaves.so proxy-mq.o -lpthread -lrt

# 5. Cliente MQ: mismo test (app-cliente.c) pero enlazado contra libproxyclaves.so
cliente_mq: app-cliente.c libproxyclaves.so claves.h
	$(CC) $(CFLAGS) -o cliente_mq app-cliente.c -L. -lproxyclaves $(RPATH)

# 6. Servidor MQ: ejecutable que usa la API local (libclaves.so)
# IMPORTANTE: enlazamos contra libclaves.so para reutilizar destroy/set/get/... ya implementadas en claves.c
servidor-mq.o: servidor-mq.c claves.h
	$(CC) $(CFLAGS) -c servidor-mq.c -o servidor-mq.o

servidor_mq: servidor-mq.o libclaves.so claves.h
	$(CC) $(CFLAGS) -o servidor_mq servidor-mq.o -L. -lclaves $(RPATH) -lrt




# REGLA PARA BORRAR TODO (incluyendo binarios de la Parte A y B)
clean:
	-@pkill servidor_mq 2>/dev/null || true
	rm -f claves.o cliente_local cliente_mq libclaves.so libproxyclaves.so proxy-mq.o servidor-mq.o servidor_mq