#ifndef _L_TEST_H_INCLUDED_
#define _L_TEST_H_INCLUDED_

//
#define t_echo(x,...) \
printf("[%s]-[%s]-[%s]-[%d]-<"#x"> ", __DATE__, __FILE__, __func__, __LINE__ );\
printf(__VA_ARGS__);\
printf("\n");


#define t_assert( x ) \
if( !(x) ) { \
t_echo( ERROR,""#x" false");\
failed_num++;\
}


#define BLOCK_MAX_NUM 1024
uint32 failed_num;

typedef void ( * test_init_pt )( void );
typedef void ( * test_pt ) ( void );
typedef struct test_t {
	test_pt  pt;
} test_t;
typedef struct manager_t {
	test_t		test[BLOCK_MAX_NUM];
	uint32			num;
} manager_t;

extern void ts_list_init( void );
extern void ts_queue_init( void );
extern void ts_bst_init( void );
extern void ts_bheap_init( void );
extern void ts_json_init( void );
extern void ts_str_init( void );
extern void ts_mem_init( void );

status test_add ( test_pt pt );
status test_run( void );
status test_init( void );
status test_end( void );

#endif
