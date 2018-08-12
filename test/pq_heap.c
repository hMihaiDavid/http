#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef NULL
 #define NULL 0
#endif

static void *xmalloc(size_t size) {
    void *p = malloc(size);
    if(p == NULL)
        err(1, "malloc() failed. Big trouble.");
    return p;
}
static void *xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size);
    if(p == NULL)
        err(1, "realloc() failed. Big trouble.");
    return p;
}
/* ------------------------------------------------------ */

/* Dummy struct for testing */
struct connection {
	time_t last_active;
};

/* ------------------------------------------------------ */

typedef struct pq_heap_t {
	size_t size; /* Number of elements */
	struct connection **data; /* Growing array that stores the heap. */
	size_t data_len; /* Length of data array. */
	size_t grow_rate; /* how many element slots to grow when more space is needed. */
} pq_heap_t;

pq_heap_t *pq_init(size_t initial_size, size_t grow_rate) {
	pq_heap_t *heap;
	
	/* Default values. */
	if(initial_size == 0) initial_size = 1000;
	if(grow_rate == 0) grow_rate = 100;
	
	heap = xmalloc(sizeof(pq_heap_t));
	heap->data = xmalloc((initial_size+1)*sizeof(struct connection*));
	heap->size = 0;
	heap->data_len = initial_size+1;
	heap->grow_rate = grow_rate;
	
	return heap;
	
}

struct connection *pq_peek(pq_heap_t *heap) {
	return heap->size > 0 ? heap->data[1] : NULL;
}

int pq_remove(pq_heap_t *heap, struct connection* conn) {
	for(size_t i = 1; i<=heap->size; i++) {
		if(heap->data[i] == conn) {
			heap->data[i] = heap->data[heap->size];
			heap->data[heap->size] = NULL;
			heap->size--;
			_pq_min_heapify(heap, i);
			_pq_decrease_key(heap, i);
			return 1;
		}
	}
	return 0;
}

void pq_add(pq_heap_t *heap, struct connection* conn) {
	if(heap->size == heap->data_len-1) _pq_realloc(heap, heap->grow_rate);
	heap->data[++heap->size] = conn;
	_pq_decrease_key(heap, heap->size);
}

/* private methods */
void _pq_realloc(pq_heap_t *heap, size_t block_len) {
	fprintf(stderr, "==> %llu\n", 
	(unsigned long long)  (heap->data_len+block_len)*sizeof(struct connection *));
	
	heap->data = xrealloc(heap->data, (heap->data_len+block_len)*sizeof(struct connection *));
	heap->data_len += block_len;
}

void _pq_min_heapify(pq_heap_t *heap, size_t i) {
	size_t l = i << 1;
	size_t r = l + 1;
	size_t smallest=i;
	
	if(l <= heap->size && heap->data[l]->last_active <  heap->data[smallest]->last_active) smallest = l;
	if(r <= heap->size && heap->data[r]->last_active <  heap->data[smallest]->last_active) smallest = r;
	
	if(smallest != i) {
		/* swap data[i] and data[smallest] */
		struct connection *tmp = heap->data[smallest];
		heap->data[smallest] = heap->data[i];
		heap->data[i] = tmp;
		
		_pq_min_heapify(heap, smallest);
	}
}

void _pq_decrease_key(pq_heap_t *heap, size_t i) {
	size_t p = i >> 1;
	while(i > 1 && heap->data[i]->last_active < heap->data[p]->last_active) {
		/* swap data[i] and data[p] */
		struct connection *tmp = heap->data[i];
		heap->data[i] = heap->data[p];
		heap->data[p] = tmp;
		
		i = p;
		p = i >> 1;
	}
}

int main() {
	
	pq_heap_t *queue;
	time_t *result;
	
	queue = pq_init(0,0);
	
	srand(time(NULL));
	
	#define N 2000
	
	for(int i=0; i<N; i++) {
		struct connection *conn = xmalloc(sizeof(struct connection));
		conn->last_active = (time_t)rand();
		pq_add(queue, conn);
	}
	
	result = xmalloc(N*sizeof(time_t));
	
	for(int i=0; i<N; i++) {
		struct connection *conn = pq_peek(queue);
		pq_remove(queue, conn);
		result[i] = conn->last_active;
		
		printf("%d:\t%llu\n", i, (unsigned long long) conn->last_active);
	}
	
	for(int i=0; i<N-1; i++) {
		if(result[i] > result[i+1]) {
			printf("====> Corrupted heap.\n");
			return 1;
		}
	}
	
	return 0;
}
