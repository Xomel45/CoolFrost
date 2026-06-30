#ifndef LIST_H
#define LIST_H

#include <stddef.h>

/* ── Intrusive doubly-linked list (Linux kernel style) ──────────────────── *
 *                                                                            *
 * Embed list_head_t inside your struct, then use list_entry() to recover    *
 * the containing struct from a pointer to the member.                       *
 *                                                                            *
 * Example:                                                                   *
 *   typedef struct { int val; list_head_t node; } item_t;                   *
 *   LIST_HEAD(my_list);                                                      *
 *   item_t a = { .val = 1 }; list_add(&a.node, &my_list);                  *
 *   list_head_t *p;                                                          *
 *   list_for_each(p, &my_list) {                                             *
 *       item_t *it = list_entry(p, item_t, node);                           *
 *   }                                                                        */

typedef struct list_head {
    struct list_head *next;
    struct list_head *prev;
} list_head_t;

/* Static initialiser — creates a self-linked sentinel head */
#define LIST_HEAD_INIT(name)  { &(name), &(name) }

/* Declare and initialise a list head in one shot */
#define LIST_HEAD(name)       list_head_t name = LIST_HEAD_INIT(name)

/* Runtime initialise */
static inline void list_init(list_head_t *h) {
    h->next = h;
    h->prev = h;
}

static inline int list_empty(const list_head_t *h) {
    return h->next == h;
}

/* ── Internal splice helper ─────────────────────────────────────────────── */
static inline void __list_add(list_head_t *n,
                               list_head_t *prev,
                               list_head_t *next)
{
    next->prev = n;
    n->next    = next;
    n->prev    = prev;
    prev->next = n;
}

/* ── Add at front (after head) ──────────────────────────────────────────── */
static inline void list_add(list_head_t *n, list_head_t *head) {
    __list_add(n, head, head->next);
}

/* ── Add at back (before head) ──────────────────────────────────────────── */
static inline void list_add_tail(list_head_t *n, list_head_t *head) {
    __list_add(n, head->prev, head);
}

/* ── Unlink a node ──────────────────────────────────────────────────────── */
static inline void list_del(list_head_t *n) {
    n->prev->next = n->next;
    n->next->prev = n->prev;
    n->next = (list_head_t *)0;
    n->prev = (list_head_t *)0;
}

/* ── Unlink and re-init (safe to re-add after this) ────────────────────── */
static inline void list_del_init(list_head_t *n) {
    list_del(n);
    list_init(n);
}

/* ── Move node to the front of another list ─────────────────────────────── */
static inline void list_move(list_head_t *n, list_head_t *head) {
    list_del(n);
    list_add(n, head);
}

/* ── Count elements (O(n)) ──────────────────────────────────────────────── */
static inline size_t list_count(const list_head_t *head) {
    size_t c = 0;
    const list_head_t *p;
    for (p = head->next; p != head; p = p->next) c++;
    return c;
}

/* ── Get containing struct from pointer to member ───────────────────────── */
#define list_entry(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/* First / last entry in a non-empty list */
#define list_first_entry(head, type, member) \
    list_entry((head)->next, type, member)

#define list_last_entry(head, type, member) \
    list_entry((head)->prev, type, member)

/* ── Iteration macros ───────────────────────────────────────────────────── */

/* Iterate over raw list_head_t pointers */
#define list_for_each(pos, head) \
    for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)

/* Safe variant — allows deleting 'pos' during iteration */
#define list_for_each_safe(pos, tmp, head) \
    for ((pos) = (head)->next, (tmp) = (pos)->next; \
         (pos) != (head); \
         (pos) = (tmp), (tmp) = (pos)->next)

/* Iterate over entries (typed) */
#define list_for_each_entry(pos, head, member)                          \
    for ((pos) = list_entry((head)->next, typeof(*(pos)), member);      \
         &(pos)->member != (head);                                       \
         (pos) = list_entry((pos)->member.next, typeof(*(pos)), member))

/* Safe typed iteration */
#define list_for_each_entry_safe(pos, tmp, head, member)                    \
    for ((pos) = list_entry((head)->next, typeof(*(pos)), member),          \
         (tmp) = list_entry((pos)->member.next, typeof(*(pos)), member);    \
         &(pos)->member != (head);                                           \
         (pos) = (tmp),                                                      \
         (tmp) = list_entry((pos)->member.next, typeof(*(pos)), member))

/* Reverse iteration */
#define list_for_each_entry_reverse(pos, head, member)                  \
    for ((pos) = list_entry((head)->prev, typeof(*(pos)), member);      \
         &(pos)->member != (head);                                       \
         (pos) = list_entry((pos)->member.prev, typeof(*(pos)), member))

#endif
