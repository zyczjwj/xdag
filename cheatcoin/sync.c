/* синхронизация, T13.738-T13.000 $DVS:time$ */

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "sync.h"
#include "hash.h"
#include "main.h"
#include "transport.h"

#define SYNC_HASH_SIZE 0x10000
#define get_list(hash) (g_sync_hash + ((hash)[0] & (SYNC_HASH_SIZE - 1)))

struct sync_block {
	struct cheatcoin_block b;
	struct sync_block *next;
	uint8_t nfield;
};

static struct sync_block **g_sync_hash;
static pthread_mutex_t g_sync_hash_mutex = PTHREAD_MUTEX_INITIALIZER;

/* заносит блок в лист ожидания, ожидается блок с хешем, записанным в поле nfield блока b */
static int push_block(struct cheatcoin_block *b, int nfield) {
	struct sync_block **p, *q;
	pthread_mutex_lock(&g_sync_hash_mutex);
	for (p = get_list(b->field[nfield].hash), q = *p; q; q = q->next) {
		if (!memcmp(&q->b, b, sizeof(struct cheatcoin_block))) {
			pthread_mutex_unlock(&g_sync_hash_mutex);
			return 0;
		}
	}
	q = (struct sync_block *)malloc(sizeof(struct sync_block));
	if (!q) return -1;
	memcpy(&q->b, b, sizeof(struct cheatcoin_block));
	q->nfield = nfield;
	q->next = *p;
	*p = q;
	g_cheatcoin_extstats.nwaitsync++;
	pthread_mutex_unlock(&g_sync_hash_mutex);
	return 1;
}

/* извещает механизм синхронизации, что искомый блок уже найден */
int cheatcoin_sync_pop_block(struct cheatcoin_block *b) {
	struct sync_block **p, *q;
	cheatcoin_hash_t hash;
	cheatcoin_hash(b, sizeof(struct cheatcoin_block), hash);
	pthread_mutex_lock(&g_sync_hash_mutex);
	for (p = get_list(hash); (q = *p);) {
		if (memcmp(hash, q->b.field[q->nfield].hash, sizeof(cheatcoin_hashlow_t))) p = &q->next;
		else {
			*p = q->next;
			g_cheatcoin_extstats.nwaitsync--;
			pthread_mutex_unlock(&g_sync_hash_mutex);
			cheatcoin_sync_add_block(&q->b, 0);
			free(q);
		}
	}
	pthread_mutex_unlock(&g_sync_hash_mutex);
	return 0;
}

/* проверить блок и включить его в базу данных с учётом синхронизации, возвращает не 0 в случае ошибки */
int cheatcoin_sync_add_block(struct cheatcoin_block *b, void *conn) {
	int res = cheatcoin_add_block(b);
	if (res >= 0) {
		cheatcoin_sync_pop_block(b);
		if (res > 0) cheatcoin_send_packet(b, (void *)((long)conn | 1l));
	} else if (((res = -res) & 0xf) == 5) {
		res = (res >> 4) & 0xf;
		push_block(b, res);
		cheatcoin_request_block(b->field[res].hash, (conn ? conn : (void *)1l));
	}
	return 0;
}

/* инициализация синхронизации блоков */
int cheatcoin_sync_init(void) {
	g_sync_hash = (struct sync_block **)calloc(sizeof(struct sync_block *), SYNC_HASH_SIZE);
	if (!g_sync_hash) return -1;
	return 0;
}
