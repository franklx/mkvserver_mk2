#include "publisher.h"
#include "debug.h"

void client_print(struct Client *c)
{
    DEBUG("State: ");
    switch(c->buffer->state) {
    case FREE:
        DEBUG("FREE");
        break;
    case RESERVED:
        DEBUG("RESERVED");
        break;
    case WAIT:
        DEBUG("WAIT");
        break;
    case WRITABLE:
        DEBUG("WRITABLE");
        break;
    case BUSY:
        DEBUG("BUSY");
        break;
    case BUFFER_FULL:
        DEBUG("BUFFER_FULL");
        break;
    default:
        DEBUG("LOL");
        break;
    }
    DEBUG("\n");
}

void client_disconnect(struct Client *c)
{
    av_write_trailer(c->ofmt_ctx);
    avio_close(c->ofmt_ctx->pb);
    avformat_free_context(c->ofmt_ctx);
    buffer_free(c->buffer);
    buffer_init(c->buffer);
    client_set_state(c, FREE);
    c->current_segment_id = -1;
    return;
}

void client_set_state(struct Client *c, enum State state)
{
    pthread_mutex_lock(&c->state_lock);
    c->state = state;
    pthread_mutex_unlock(&c->state_lock);
}

void publisher_init(struct PublisherContext **pub)
{
    int i;
    *pub = (struct PublisherContext*) malloc(sizeof(struct PublisherContext));
    (*pub)->nb_threads = 4;
    (*pub)->current_segment_id = -1;
    (*pub)->buffer = (struct Buffer*) malloc(sizeof(struct Buffer));
    (*pub)->fs_buffer = (struct Buffer*) malloc(sizeof(struct Buffer));
    (*pub)->shutdown = 0;
    buffer_init((*pub)->buffer);
    buffer_init((*pub)->fs_buffer);
    for(i = 0; i < MAX_CLIENTS; i++) {
        struct Client *c = &(*pub)->subscribers[i];
        c->buffer = (struct Buffer*) malloc(sizeof(struct Buffer));
        buffer_init(c->buffer);
        c->id = i;
        c->current_segment_id = -1;
        pthread_mutex_init(&c->state_lock, NULL);
        client_set_state(c, FREE);
    }

    return;
}

int publisher_reserve_client(struct PublisherContext *pub)
{
    int i;
    for(i = 0; i < MAX_CLIENTS; i++) {
        switch(pub->subscribers[i].buffer->state) {
        case FREE:
            buffer_set_state(pub->subscribers[i].buffer, RESERVED);
            return 0;
        default:
            continue;
        }
    }
    return 1;
}

void publisher_cancel_reserve(struct PublisherContext *pub)
{
    int i;
    for(i = 0; i < MAX_CLIENTS; i++) {
        switch(pub->subscribers[i].buffer->state) {
        case RESERVED:
            buffer_set_state(pub->subscribers[i].buffer, FREE);
            return;
        default:
            continue;
        }
    }
    return;
}

void publisher_add_client(struct PublisherContext *pub, AVFormatContext *ofmt_ctx)
{
    int i, j;
    struct Segment *prebuffer_seg;
    for(i = 0; i < MAX_CLIENTS; i++) {
        switch(pub->subscribers[i].buffer->state) {
        case RESERVED:
            DEBUG("Put new client at %d, ofmt_ctx: %p pb: %p\n", i, ofmt_ctx, ofmt_ctx->pb);
            pub->subscribers[i].ofmt_ctx = ofmt_ctx;
            buffer_set_state(pub->subscribers[i].buffer, WRITABLE);
            client_set_state(&pub->subscribers[i], WRITABLE);
            for (j = 0; j < BUFFER_SEGMENTS; j++) {
                if ((prebuffer_seg = buffer_get_segment_at(pub->fs_buffer, pub->fs_buffer->read + j))) {
                    buffer_push_segment(pub->subscribers[i].buffer, prebuffer_seg);
                    DEBUG("pushed prebuffer segment.\n");
                }
            }
            return;
        default:
            continue;
        }
    }
}


void publisher_free(struct PublisherContext *pub)
{
    int i;
    buffer_free(pub->buffer);
    buffer_free(pub->fs_buffer);
    for(i = 0; i < MAX_CLIENTS; i++) {
        struct Client *c = &pub->subscribers[i];
        buffer_free(c->buffer);
    }
    return;
}

void publish(struct PublisherContext *pub)
{
    int i;
    struct Segment *seg = buffer_peek_segment(pub->buffer);
    if (seg) {
        pub->current_segment_id = seg->id;
        for (i = 0; i < MAX_CLIENTS; i++) {
            switch(pub->subscribers[i].buffer->state) {
            case BUFFER_FULL:
                fprintf(stderr, "Warning: dropping segment for client %d\n", i);
                continue;
            case WAIT:
            case WRITABLE:
                buffer_push_segment(pub->subscribers[i].buffer, seg);
                continue;
            default:
                continue;

            }
        }
        buffer_push_segment(pub->fs_buffer, seg);
    }
    buffer_drop_segment(pub->buffer);
    if (pub->fs_buffer->nb_segs == BUFFER_SEGMENTS) {
        buffer_drop_segment(pub->fs_buffer);
        DEBUG("Dropped segment from prebuffer buffer.\n");
    }
}

char *publisher_gen_status_json(struct PublisherContext *pub)
{
    int nb_free = 0, nb_reserved = 0, nb_wait = 0, nb_writable = 0, nb_busy = 0, nb_buffer_full = 0, current_read = 0, newest_write = 0, oldest_write = 0;
    int i;
    struct Client *c;
    char *status = (char*) malloc(sizeof(char)* 4096);


    current_read = pub->current_segment_id;
    oldest_write = current_read;

    for (i = 0; i < MAX_CLIENTS; i++) {
        c = &pub->subscribers[i];
        if (c->current_segment_id > 0 && c->current_segment_id < oldest_write) {
            oldest_write = c->current_segment_id;
        }
        if (c->current_segment_id > newest_write) {
            newest_write = c->current_segment_id;
        }

        switch(c->buffer->state) {
        case FREE:
            nb_free++;
            continue;
        case RESERVED:
            nb_reserved++;
            continue;
        case WAIT:
            nb_wait++;
            continue;
        case WRITABLE:
            nb_writable++;
            continue;
        case BUSY:
            nb_busy++;
            continue;
        case BUFFER_FULL:
            nb_buffer_full++;
            continue;
        default:
            continue;
        }
    }


    snprintf(status, 4095, "{\n\t\"free\": %d,\n\t\"reserved\": %d,\n\t\"wait\": %d,\n\t\"writable\": %d,\n\t\"busy\": %d\n\t\"buffer_full\": %d\n\t\"current_read\": %d\n\t\"newest_write\": %d\n\t\"oldest_write\": %d\n}\n", nb_free, nb_reserved, nb_wait, nb_writable, nb_busy, nb_buffer_full, current_read, newest_write, oldest_write);
    return status;
}
