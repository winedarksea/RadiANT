/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Fake <ti/drivers/utils/List.h>: the doubly linked list TI's RF driver hands
 * to the scheduler-policy hooks (radiant_radio_cc26xx_arb.c walks the pend
 * queue with it). Same names and shapes as hal_ti's; static inline here so the
 * test needs no List.c.
 */
#ifndef FAKE_TI_DRIVERS_UTILS_LIST_H_
#define FAKE_TI_DRIVERS_UTILS_LIST_H_

#include <stdbool.h>
#include <stddef.h>

typedef struct List_Elem {
	struct List_Elem *next;
	struct List_Elem *prev;
} List_Elem;

typedef struct List_List {
	List_Elem *head;
	List_Elem *tail;
} List_List;

static inline void List_clearList(List_List *list)
{
	list->head = NULL;
	list->tail = NULL;
}

static inline bool List_empty(List_List *list)
{
	return list->head == NULL;
}

static inline List_Elem *List_head(List_List *list)
{
	return list->head;
}

static inline List_Elem *List_tail(List_List *list)
{
	return list->tail;
}

static inline List_Elem *List_next(List_Elem *elem)
{
	return elem->next;
}

static inline List_Elem *List_prev(List_Elem *elem)
{
	return elem->prev;
}

static inline void List_put(List_List *list, List_Elem *elem)
{
	elem->next = NULL;
	elem->prev = list->tail;
	if (list->tail != NULL) {
		list->tail->next = elem;
	} else {
		list->head = elem;
	}
	list->tail = elem;
}

static inline void List_putHead(List_List *list, List_Elem *elem)
{
	elem->prev = NULL;
	elem->next = list->head;
	if (list->head != NULL) {
		list->head->prev = elem;
	} else {
		list->tail = elem;
	}
	list->head = elem;
}

static inline void List_remove(List_List *list, List_Elem *elem)
{
	if (elem->prev != NULL) {
		elem->prev->next = elem->next;
	} else {
		list->head = elem->next;
	}
	if (elem->next != NULL) {
		elem->next->prev = elem->prev;
	} else {
		list->tail = elem->prev;
	}
	elem->next = NULL;
	elem->prev = NULL;
}

static inline List_Elem *List_get(List_List *list)
{
	List_Elem *e = list->head;

	if (e != NULL) {
		List_remove(list, e);
	}
	return e;
}

#endif /* FAKE_TI_DRIVERS_UTILS_LIST_H_ */
