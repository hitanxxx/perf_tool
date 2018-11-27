#include "lk.h"

// l_mem_page_create -------
status l_mem_page_create( l_mem_page_t ** alloc, uint32 size )
{
    char * p = NULL;
    uint32 length = 0;
    l_mem_page_t * page;

    length = (size > L_PAGE_SIZE) ? size : L_PAGE_SIZE;
    p = l_safe_malloc( length + sizeof(l_mem_page_t) );
    if( !p ) {
        err_log("%s --- l_safe_malloc failed, [%d]", __func__, errno );
        return ERROR;
    }
    memset( p, 0, length + sizeof(l_mem_page_t) );

    page = (l_mem_page_t*)p;
    page->data = p + sizeof(l_mem_page_t);
    page->start = page->data;
    page->end = page->start + length;
    page->next = NULL;
    *alloc = page;
    return OK;
}
// l_mem_page_free ------
status l_mem_page_free( l_mem_page_t * page )
{
    l_mem_page_t * n;

    while( page ) {
        n = page->next;
        l_safe_free( page );
        page = n;
    }
    return OK;
}
// l_mem_alloc -------
void * l_mem_alloc( l_mem_page_t * page, uint32 size )
{
    l_mem_page_t * q = NULL, *cl = NULL;
    l_mem_page_t * new = NULL;
    uint32 length = 0;
    char * p;

    if( size > L_PAGE_SIZE ) {
        if( OK != l_mem_page_create( &new, size ) ) {
            err_log("%s --- create page failed", __func__ );
            return NULL;
        }

        cl = page;
        q = cl;
        while( cl ) {
            q = cl;
            cl = cl->next;
        }
        q->next = new;
        new->start += size;
        return new->data;
    } else {
        cl = page;
        q = cl;
        while( cl ) {
            p = cl->start;
            if( (uint32)(cl->end - p) >= size ) {
                cl->start += size;
                return p;
            }
            q = cl;
            cl = cl->next;
        }
        if( OK != l_mem_page_create( &new, L_PAGE_SIZE ) ) {
            err_log("%s --- create page failed", __func__ );
            return NULL;
        }
        q->next = new;
        new->start += size;
        return new->data;
    }
}
