#include <stdio.h>
#include <stdlib.h>  // malloc
#include <string.h>  // memset
#include <assert.h>  // assert
#include <errno.h>
#include <time.h>
#include <unistd.h>  // close
#include <pthread.h>

#include <arpa/inet.h>

#include "request.h"
#include "buffer.h"

#include "stm.h"
#include "passive.h"
#include"netutils.h"

#define N(x) (sizeof(x)/sizeof((x)[0]))


typedef enum {

    /**
     * recibe el mensaje `request` del cliente, y lo inicia su proceso
     *
     * Intereses:
     *     - OP_READ sobre client_fd
     *
     * Transiciones:
     *   - REQUEST_READ        mientras el mensaje no esté completo
     *   - REQUEST_RESOLV      si requiere resolver un nombre DNS
     *   - REQUEST_CONNECTING  si no require resolver DNS, y podemos iniciar
     *                         la conexión al origin server.
     *   - REQUEST_WRITE       si determinamos que el mensaje no lo podemos
     *                         procesar (ej: no soportamos un comando)
     *   - ERROR               ante cualquier error (IO/parseo)
     */
    REQUEST_READ,

    /**
     * Espera la resolución DNS
     *
     * Intereses:
     *     - OP_NOOP sobre client_fd. Espera un evento de que la tarea bloqueante
     *               terminó.
     * Transiciones:
     *     - REQUEST_CONNECTING si se logra resolución al nombre y se puede
     *                          iniciar la conexión al origin server.
     *     - REQUEST_WRITE      en otro caso
     */
    REQUEST_RESOLV,

    /**
     * Espera que se establezca la conexión al origin server
     *
     * Intereses:
     *    - OP_WRITE sobre client_fd
     *
     * Transiciones:
     *    - REQUEST_WRITE    se haya logrado o no establecer la conexión.
     *
     */
    REQUEST_CONNECTING,

    /**
     * envía la respuesta del `request' al cliente.
     *
     * Intereses:
     *   - OP_WRITE sobre client_fd
     *   - OP_NOOP  sobre origin_fd
     *
     * Transiciones:
     *   - COPY         si el request fue exitoso y tenemos que copiar el
     *                  contenido de los descriptores
     *   - ERROR        ante I/O error
     */
    REQUEST_WRITE,

    // estados terminales
    DONE,
    ERROR,
} sock_state_t;

typedef enum {
    RESPONSE_READ,
    RESPONSE_TRANSFORM,
    RESPONSE_WRITE,
    RESPONSE_DONE,
    RESPONSE_ERROR,
} origin_state_t;


/** usado por REQUEST_READ, REQUEST_WRITE, REQUEST_RESOLV */
typedef struct {
    /** buffer utilizado para I/O */
    buffer                    *rb, *wb;

    /** parser */
    struct request             request;
    struct request_parser      parser;

    /** el resumen del respuesta a enviar*/
    enum socks_response_status status;

    // ¿a donde nos tenemos que conectar?
    struct sockaddr_storage   *origin_addr;
    socklen_t                 *origin_addr_len;
    int                       *origin_domain;

    const int                 *client_fd;
    int                       *origin_fd;
} request_st;

/** usado por REQUEST_CONNECTING */
struct connecting {
    buffer     *wb;
    const int  *client_fd;
    int        *origin_fd;
    enum socks_response_status *status;
};

typedef struct {
    /** información del cliente */
    struct sockaddr_storage       client_addr;
    socklen_t                     client_addr_len;
    int                           client_fd;

    /** resolución de la dirección del origin server */
    struct addrinfo              *origin_resolution;

    /** información del origin server */
    struct sockaddr_storage       origin_addr;
    socklen_t                     origin_addr_len;
    int                           origin_domain;
    int                           origin_fd;

    /** maquinas de estados */
    struct state_machine          stm;

    /** estados para el client_fd */
    union {
        request_st         request;
    } client;
    /** estados para el origin_fd */
    union {
        struct connecting         conn;
    } orig;

    /** buffers para ser usados read_buffer, write_buffer.*/
    uint8_t raw_buff_a[2048], raw_buff_b[2048];
    buffer read_buffer, write_buffer;

} client_t;

typedef struct {
    int origin_fd;
    int client_fd;

    char * response;
    uint8_t length;

    struct state_machine stm;
} origin_t;


#define ATTACHMENT(key) ( (client_t *)(key)->data)

static void
request_read_close(const unsigned state, struct selector_key *key);

static void
request_init(const unsigned state, struct selector_key *key);

static unsigned
request_read(struct selector_key *key);

static unsigned
request_resolv(struct selector_key * key, request_st * d);

static unsigned
request_resolv_done(struct selector_key *key);

static void
request_connecting_init(const unsigned state, struct selector_key *key);

static unsigned
request_connecting(struct selector_key *key);

static unsigned
request_connect(struct selector_key *key, request_st *d);

static unsigned
request_write(struct selector_key *key);

static void
response_init(const unsigned state, struct selector_key *key);

//static unsigned
//response_read_close(struct selector_key *key);

static unsigned
response_read(struct selector_key *key);

static unsigned
response_write(struct selector_key *key);

static const struct state_definition origin_statbl[] = {
    {
        .state            = RESPONSE_READ,
        .on_arrival       = response_init,              //TODO
    //    .on_departure     = response_read_close,        //TODO
        .on_read_ready    = response_read,              //TODO
    },{
        .state            = RESPONSE_TRANSFORM,
    //    .on_block_ready   = response_transform_done,    //TODO
    },{
        .state            = RESPONSE_WRITE,
        .on_arrival       = response_write,             //TODO
    },{
        .state            = RESPONSE_DONE,

    },{
        .state            = RESPONSE_ERROR,
    }
};

static const struct state_definition client_statbl[] = {
    {
        .state            = REQUEST_READ,
        .on_arrival       = request_init,
        .on_departure     = request_read_close,
        .on_read_ready    = request_read,
    },{
        .state            = REQUEST_RESOLV,
        .on_block_ready   = request_resolv_done,
    },{
        .state            = REQUEST_CONNECTING,
        .on_arrival       = request_connecting_init,
        .on_write_ready   = request_connecting,
    },{
        .state            = REQUEST_WRITE,
        .on_arrival       = request_write,
    },{
        .state            = DONE,

    },{
        .state            = ERROR,
    }
};

static const struct state_definition * origin_describe_states(void){
    return origin_statbl;
}

static const struct state_definition * client_describe_states(void) {
    return client_statbl;
}

static origin_t * origin_new(int origin_fd, int client_fd) {
    origin_t * ret;

    ret = malloc(sizeof(*ret));

    if(ret == NULL) {
        return NULL;
    }
    memset(ret, 0x00, sizeof(*ret));

    ret->origin_fd = origin_fd;
    ret->client_fd = client_fd;

    ret->stm.initial = RESPONSE_READ;
    ret->stm.max_state = RESPONSE_ERROR;
    ret->stm.states = origin_describe_states();
    stm_init(&ret->stm);

    return ret;
}

static client_t * client_new(int client_fd) {
    client_t *ret;

    ret = malloc(sizeof(*ret));

    if(ret == NULL) {
        goto finally;
    }
    memset(ret, 0x00, sizeof(*ret));

    ret->origin_fd       = -1;
    ret->client_fd       = client_fd;
    ret->client_addr_len = sizeof(ret->client_addr);

    ret->stm    .initial   = REQUEST_READ;
    ret->stm    .max_state = ERROR;
    ret->stm    .states    = client_describe_states();
    stm_init(&ret->stm);

    buffer_init(&ret->read_buffer,  N(ret->raw_buff_a), ret->raw_buff_a);
    buffer_init(&ret->write_buffer, N(ret->raw_buff_b), ret->raw_buff_b);

finally:
    return ret;
}

static void sock_destroy(client_t* s) {
    if(s->origin_resolution != NULL) {
        //freeaddrinfo(s->origin_resolution);
        s->origin_resolution = 0;
    }
    //free(s);
}

static void sock_read(struct selector_key *key);
static void sock_write(struct selector_key *key);
static void sock_block(struct selector_key *key);
static void sock_close(struct selector_key *key);
static const struct fd_handler socks_handler = {
    .handle_read   = sock_read,
    .handle_write  = sock_write,
    .handle_close  = sock_close,
    .handle_block  = sock_block,
};


void socks_passive_accept(struct selector_key *key){
    struct sockaddr_storage client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    client_t * state = NULL;

    const int client = accept(key->fd, (struct sockaddr*) &client_addr, &client_addr_len);

    if(client == -1){
        return;
    }

    if(selector_fd_set_nio(client) == -1){
        return;
    }


    state = client_new(client);
    if(state == NULL){
        goto fail;
    }

    memcpy(&state->client_addr, &client_addr, client_addr_len);
    state->client_addr_len = client_addr_len;

    if(selector_register(key->s, client, &socks_handler, OP_READ, state) != SELECTOR_SUCCESS){
        goto fail;
    }

    return;


fail:
    if(client != -1) {
        close(client);
    }
    sock_destroy(state);
}

static void sock_done(struct selector_key* key);

static void sock_read(struct selector_key *key) {
    struct state_machine *stm   = &ATTACHMENT(key)->stm;
    const sock_state_t st = stm_handler_read(stm, key);

    if(ERROR == st || DONE == st) {
        sock_done(key);
    }
}

static void sock_write(struct selector_key *key) {
    struct state_machine *stm   = &ATTACHMENT(key)->stm;
    const sock_state_t st = stm_handler_write(stm, key);

    if(ERROR == st || DONE == st) {
        sock_done(key);
    }
}

static void sock_block(struct selector_key *key) {
    struct state_machine *stm   = &ATTACHMENT(key)->stm;
    const sock_state_t st = stm_handler_block(stm, key);

    if(ERROR == st || DONE == st) {
        sock_done(key);
    }
}

static void sock_close(struct selector_key *key) {
    sock_destroy(ATTACHMENT(key));
}

static void sock_done(struct selector_key* key) {
    const int fds[] = {
        ATTACHMENT(key)->client_fd,
        ATTACHMENT(key)->origin_fd,
    };
    for(unsigned i = 0; i < N(fds); i++) {
        if(fds[i] != -1) {
            if(SELECTOR_SUCCESS != selector_unregister_fd(key->s, fds[i])) {
                abort();
            }
            close(fds[i]);
        }
    }
}

static void response_init(const unsigned state, struct selector_key *key) {
    origin_t * o = (origin_t*) key->data;

    printf("called response\n");

    o->response = malloc(4096);
    o->length = 0;
}

static void
request_init(const unsigned state, struct selector_key *key) {
    request_st * d = &ATTACHMENT(key)->client.request;

    d->rb              = &(ATTACHMENT(key)->read_buffer);
    d->wb              = &(ATTACHMENT(key)->write_buffer);
    d->parser.request  = &d->request;
    d->status          = status_general_SOCKS_server_failure;
    //d->request.dest_port = htons(80);
    request_parser_init(&d->parser);
    d->client_fd       = &ATTACHMENT(key)->client_fd;
    d->origin_fd       = &ATTACHMENT(key)->origin_fd;

    d->origin_addr     = &ATTACHMENT(key)->origin_addr;
    d->origin_addr_len = &ATTACHMENT(key)->origin_addr_len;
    d->origin_domain   = &ATTACHMENT(key)->origin_domain;
}

static unsigned response_read(struct selector_key *key){
    origin_t * o = (origin_t*) key->data;

    o->length = recv(key->fd, o->response, 4096, 0);
    if(o->length > 0){
        printf("Read: %d\n", o->length);
        printf("%s\n", o->response);
        return RESPONSE_WRITE;
    }
    return RESPONSE_ERROR;
}

static unsigned response_write(struct selector_key *key){
    origin_t * o = (origin_t*) key->data;

    int sent = send(o->client_fd, o->response, o->length, 0);
    return RESPONSE_DONE;
}

static unsigned
request_read(struct selector_key *key) {
    request_st * d = &ATTACHMENT(key)->client.request;

      buffer *b     = d->rb;
    unsigned  ret   = REQUEST_READ;
        bool  error = false;
     uint8_t *ptr;
      size_t  count;
     ssize_t  n;

    ptr = buffer_write_ptr(b, &count);
    n = recv(key->fd, ptr, count, 0);
    if(n > 0) {
        buffer_write_adv(b, n);
        int st = request_consume(b, &d->parser, &error);
        if(request_is_done( &d->parser, st, 0)) {
             ret = request_resolv(key, d);
        }
    } else {
        ret = ERROR;
    }

    return error ? ERROR : ret;
}

static void *
request_resolv_blocking(void *data) {
    struct selector_key *key = (struct selector_key *) data;
    client_t       *s   = ATTACHMENT(key);

    pthread_detach(pthread_self());
    s->origin_resolution = 0;
    struct addrinfo hints = {
        .ai_family    = AF_UNSPEC,    /* Allow IPv4 or IPv6 */
        .ai_socktype  = SOCK_STREAM,  /* Datagram socket */
        .ai_flags     = AI_PASSIVE,   /* For wildcard IP address */
        .ai_protocol  = 0,            /* Any protocol */
        .ai_canonname = NULL,
        .ai_addr      = NULL,
        .ai_next      = NULL,
    };

    char buff[7];
    snprintf(buff, sizeof(buff), "%d",
             ntohs(s->client.request.request.dest_port));

    getaddrinfo(s->client.request.request.host, buff, &hints,&s->origin_resolution);

    selector_notify_block(key->s, key->fd);

    free(data);

    return 0;
}

static unsigned
request_resolv(struct selector_key * key, request_st * d) {
    unsigned ret;
    pthread_t tid;

    struct selector_key* k = malloc(sizeof(*key));
    if(k == NULL) {
        ret       = REQUEST_WRITE;
        d->status = status_general_SOCKS_server_failure;
        selector_set_interest_key(key, OP_WRITE);
    } else {
        memcpy(k, key, sizeof(*k));
        if(-1 == pthread_create(&tid, 0,
                        request_resolv_blocking, k)) {
            ret       = REQUEST_WRITE;
            d->status = status_general_SOCKS_server_failure;
            selector_set_interest_key(key, OP_WRITE);
        } else{
            ret = REQUEST_RESOLV;
            selector_set_interest_key(key, OP_NOOP);
        }
    }
    return ret;
}

static unsigned
request_resolv_done(struct selector_key *key) {
    request_st * d = &ATTACHMENT(key)->client.request;
    client_t *s      =  ATTACHMENT(key);


    if(s->origin_resolution == 0) {
        d->status = status_general_SOCKS_server_failure;
    } else {
        s->origin_domain   = s->origin_resolution->ai_family;
        s->origin_addr_len = s->origin_resolution->ai_addrlen;
        memcpy(&s->origin_addr,
                s->origin_resolution->ai_addr,
                s->origin_resolution->ai_addrlen);
        freeaddrinfo(s->origin_resolution);
        s->origin_resolution = 0;
    }

    return request_connect(key, d);
}

static void
request_read_close(const unsigned state, struct selector_key *key) {
    request_st * d = &ATTACHMENT(key)->client.request;

    request_close(&d->parser);
}

static void
request_connecting_init(const unsigned state, struct selector_key *key) {
    struct connecting *d  = &ATTACHMENT(key)->orig.conn;

    d->client_fd = &ATTACHMENT(key)->client_fd;
    d->origin_fd = &ATTACHMENT(key)->origin_fd;
    d->status    = &ATTACHMENT(key)->client.request.status;
    d->wb        = &ATTACHMENT(key)->write_buffer;
}

/** la conexión ha sido establecida (o falló)  */
static unsigned
request_connecting(struct selector_key *key) {
    int error;
    socklen_t len = sizeof(error);
    struct connecting *d  = &ATTACHMENT(key)->orig.conn;

    if (getsockopt(key->fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
        *d->status = status_general_SOCKS_server_failure;
    } else {
        if(error == 0) {
            *d->status     = status_succeeded;
            *d->origin_fd = key->fd;


            origin_t * state = origin_new(*d->origin_fd, *d->client_fd);

            if(state == NULL){
                error = true;
            }

            int st = selector_unregister_fd(key->s, *d->origin_fd);

            if(st != SELECTOR_SUCCESS){
                error = true;
            }

            st = selector_register(key->s, *d->origin_fd, &socks_handler, OP_NOOP, state);

            if(st != SELECTOR_SUCCESS){
                error = true;
            }

        } else {
            *d->status = errno_to_socks(error);
        }
    }

    /*
    if(-1 == request_marshall(d->wb, *d->status)) {
        *d->status = status_general_SOCKS_server_failure;
        abort(); // el buffer tiene que ser mas grande en la variable
    }
    */
    selector_status s = 0;
    s |= selector_set_interest    (key->s, *d->client_fd, OP_WRITE);
    //s |= selector_set_interest_key(key,                   OP_NOOP);

    return error==false ? REQUEST_WRITE : ERROR;
}

static unsigned
request_write(struct selector_key *key) {
    request_st * d = &ATTACHMENT(key)->client.request;
    client_t * s = ATTACHMENT(key);

    printf("IM CALLED IN WRITE\n");

    unsigned  ret       = REQUEST_READ;
      buffer *b         = d->wb;
     uint8_t *ptr;
      size_t  count;
     ssize_t  n;

    n = send(s->origin_fd, d->request.request, d->request.length, MSG_NOSIGNAL);
    if(n == -1) {
        ret = ERROR;
    }/* else {
        buffer_read_adv(b, n);

        if() {
            if(d->status == status_succeeded) {
                ret = REQUEST_READ;
                selector_set_interest    (key->s,  *d->client_fd, OP_READ);
                selector_set_interest    (key->s,  *d->origin_fd, OP_READ);
            } else {
                ret = DONE;
                selector_set_interest    (key->s,  *d->client_fd, OP_NOOP);
                if(-1 != *d->origin_fd) {
                    selector_set_interest    (key->s,  *d->origin_fd, OP_NOOP);
                }
            }
        }
    }*/

    //log_request(d->status, (const struct sockaddr *)&ATTACHMENT(key)->client_addr,
    //                       (const struct sockaddr *)&ATTACHMENT(key)->origin_addr);

    return ret;
}

static unsigned
request_connect(struct selector_key *key, request_st *d) {
    bool error                  = false;
    // da legibilidad
    enum socks_response_status status =  d->status;
    int *fd                           =  d->origin_fd;

    *fd = socket(ATTACHMENT(key)->origin_domain, SOCK_STREAM, 0);
    if (*fd == -1) {
        error = true;
        goto finally;
    }
    if (selector_fd_set_nio(*fd) == -1) {
        goto finally;
    }
    struct sockaddr_in * originaddr = (struct sockaddr_in *)&ATTACHMENT(key)->origin_addr;
    originaddr->sin_port = htons(80);
    if (-1 == connect(*fd, (struct sockaddr_in *)&ATTACHMENT(key)->origin_addr,
                           ATTACHMENT(key)->origin_addr_len)) {
        if(errno == EINPROGRESS) {
            // es esperable,  tenemos que esperar a la conexión

            // dejamos de de pollear el socket del cliente
            selector_status st = selector_set_interest_key(key, OP_NOOP);
            if(SELECTOR_SUCCESS != st) {
                error = true;
                goto finally;
            }

            // esperamos la conexion en el nuevo socket
            st = selector_register(key->s, *fd, &socks_handler,
                                      OP_WRITE, key->data);
            if(SELECTOR_SUCCESS != st) {
                error = true;
                goto finally;
            }
        } else {
            status = errno_to_socks(errno);
            error = true;
            goto finally;
        }
    } else {
        // estamos conectados sin esperar... no parece posible
        // saltaríamos directamente a COPY
        abort();
    }

finally:
    if (error) {
        if (*fd != -1) {
            close(*fd);
            *fd = -1;
        }
    }

    d->status = status;

    return REQUEST_CONNECTING;
}