#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

// define the column size
#define COLUMN_USERNAME_SIZE 32
#define COLUMN_EMAIL_SIZE 255

/* define the rows layout */
typedef struct {
	uint32_t id;
	char username[COLUMN_USERNAME_SIZE + 1];
	char email[COLUMN_EMAIL_SIZE + 1];
} Row;

// safely measure the size of a structure's attributes
#define size_of_attrubutes(Struct, Attribute) sizeof(((Struct*)0)->Attribute)
// layout of a searialized row
/*
 	| column     | size | offset |
	------------------------------
	| id	     |   4  |    0   |
	------------------------------
	| username   |  32  |    4   |
	------------------------------
	| email	     | 255  |   36   |
	------------------------------
	| total	     | 291  |        |
*/

const uint32_t ID_SIZE = size_of_attrubutes(Row, id);
const uint32_t USERNAME_SIZE = size_of_attrubutes(Row, username);
const uint32_t EMAIL_SIZE = size_of_attrubutes(Row, email);
const uint32_t ROW_SIZE = ID_SIZE + USERNAME_SIZE + EMAIL_SIZE;

const uint32_t ID_OFFSET = 0;
const uint32_t USERNAME_OFFSET = ID_OFFSET + ID_SIZE;
const uint32_t EMAIL_OFFSET = USERNAME_OFFSET + USERNAME_SIZE;

/* define the table layout */
#define TABLE_MAX_PAGES 100
const uint32_t PAGE_SIZE = 4096; // 4k bytes

/* Node Header Format */
typedef enum { NODE_INTERNAL, NODE_LEAF } NodeType;

/* Common Node Header Layout */
const uint32_t NODE_TYPE_SIZE = sizeof(uint8_t);
const uint32_t NODE_TYPE_OFFSET = 0;
const uint32_t IS_ROOT_SIZE = sizeof(uint8_t);
const uint32_t IS_ROOT_OFFSET = NODE_TYPE_SIZE;
const uint32_t PARENT_POINTER_SIZE = sizeof(uint32_t);
const uint32_t PARENT_POINTER_OFFSET = IS_ROOT_OFFSET + IS_ROOT_SIZE;
const uint32_t COMMON_NODE_HEADER_SIZE = NODE_TYPE_SIZE + IS_ROOT_SIZE + PARENT_POINTER_SIZE;

/* Leaf Node Header Layout */
const uint32_t LEAF_NODE_NUM_CELLS_SIZE = sizeof(uint32_t);
const uint32_t LEAF_NODE_NUM_CELLS_OFFSET = COMMON_NODE_HEADER_SIZE;
const uint32_t LEAF_NODE_NEXT_LEAF_SIZE = sizeof(uint32_t);
const uint32_t LEAF_NODE_NEXT_LEAF_OFFSET = LEAF_NODE_NUM_CELLS_OFFSET + LEAF_NODE_NUM_CELLS_SIZE;
const uint32_t LEAF_NODE_HEADER_SIZE = COMMON_NODE_HEADER_SIZE + LEAF_NODE_NUM_CELLS_SIZE + LEAF_NODE_NEXT_LEAF_SIZE;

/* Leaf Node Body Layout */
const uint32_t LEAF_NODE_KEY_SIZE = sizeof(uint32_t);
const uint32_t LEAF_NODE_KEY_OFFSET = 0;
const uint32_t LEAF_NODE_VALUE_SIZE = ROW_SIZE;
const uint32_t LEAF_NODE_VALUE_OFFSET = LEAF_NODE_KEY_OFFSET + LEAF_NODE_KEY_SIZE;
const uint32_t LEAF_NODE_CELL_SIZE = LEAF_NODE_KEY_SIZE + LEAF_NODE_VALUE_SIZE;
const uint32_t LEAF_NODE_SPACE_FOR_CELLS = PAGE_SIZE - LEAF_NODE_HEADER_SIZE;
const uint32_t LEAF_NODE_MAX_CELLS = LEAF_NODE_SPACE_FOR_CELLS / LEAF_NODE_CELL_SIZE;

/* Leaf Node Size */
// 2つの新しいノードの間でセルを均等に分配
// N+1が奇数の場合、左のノードを任意に選んでセルを1つ増やすことにしている
const uint32_t LEAF_NODE_RIGHT_SPLIT_COUNT = (LEAF_NODE_MAX_CELLS + 1) / 2;
const uint32_t LEAF_NODE_LEFT_SPLIT_COUNT = (LEAF_NODE_MAX_CELLS + 1) - LEAF_NODE_RIGHT_SPLIT_COUNT;

/* Internal Node Header Layout */
const uint32_t INTERNAL_NODE_NUM_KEYS_SIZE = sizeof(uint32_t);
const uint32_t INTERNAL_NODE_NUM_KEYS_OFFSET = COMMON_NODE_HEADER_SIZE;
const uint32_t INTERNAL_NODE_RIGHT_CHILD_SIZE = sizeof(uint32_t);
const uint32_t INTERNAL_NODE_RIGHT_CHILD_OFFSET =
    INTERNAL_NODE_NUM_KEYS_OFFSET + INTERNAL_NODE_NUM_KEYS_SIZE;
const uint32_t INTERNAL_NODE_HEADER_SIZE = COMMON_NODE_HEADER_SIZE +
                                           INTERNAL_NODE_NUM_KEYS_SIZE +
                                           INTERNAL_NODE_RIGHT_CHILD_SIZE;

/* Internal Node Body Layout */
const uint32_t INTERNAL_NODE_KEY_SIZE = sizeof(uint32_t);
const uint32_t INTERNAL_NODE_CHILD_SIZE = sizeof(uint32_t);
const uint32_t INTERNAL_NODE_CELL_SIZE = INTERNAL_NODE_CHILD_SIZE + INTERNAL_NODE_KEY_SIZE;

const uint32_t INTERNAL_NODE_MAX_CELLS = 3;

typedef struct {
	int file_descriptor;
	uint32_t file_length;
	uint32_t num_pages;
	void* pages[TABLE_MAX_PAGES];
} Pager;

typedef struct {
	// 全体の行数を保持
	uint32_t num_rows;
	Pager* pager;
	uint32_t root_page_num;
} Table;

// テーブル内の場所を表すオブジェクト
typedef struct {
	Table* table;
	uint32_t page_num;
	uint32_t cell_num;
	// 次に行を挿入したい場所。最後の要素の1つ後ろの位置を示す
	bool end_of_table;
} Cursor;

typedef struct {
	char* buffer;
	size_t buffer_length;
	size_t input_length;
} InputBuffer;

typedef enum {
	META_COMMAND_SUCCESS,
	META_COMMAND_UNRECOGNIZED_COMMAND
} MetaCommandResult;

typedef enum {
	PREPARE_SUCCESS,
	PREPARE_UNRECOGNIZED_STATEMENT,
	PREPARE_SYNTAX_ERROR,
	PREPARE_STRING_TOO_LONG,
	PREPARE_NEGATIVE_ID
} PrepareResult;

typedef enum {
	STATEMENT_INSERT,
	STATEMENT_SELECT
} StatementType;

typedef enum {
	EXECUTE_SUCCESS,
	EXECUTE_TABLE_FULL,
	EXECUTE_DUPLICATE_KEY
} ExecuteResult;

typedef struct {
	StatementType type;
	// sscanfによって解析されたメンバの変数を持ったRowを格納
	Row row_to_insert;
} Statement;

static InputBuffer* new_input_buffer();
static void read_input(InputBuffer* buffer);
static void close_input_buffer(InputBuffer* input_buffer);
static void print_prompt();
static MetaCommandResult do_meta_command(InputBuffer* input_buffer, Table* table);
static PrepareResult prepare_statement(InputBuffer* input_buffer, Statement* statement);
static PrepareResult prepare_insert(InputBuffer* input_buffer, Statement* statement);
static ExecuteResult execute_statement(Statement* statement, Table* table);
static ExecuteResult execute_insert(Statement* statement, Table* table);
static ExecuteResult execute_select(Statement* statement, Table* table);
static void serialize_row(Row* source, void* destination);
static void deserialize_row(void* source, Row* destination);
static void* cursor_value(Cursor* cursor);
static void print_row(Row* row);
static Table* db_open();
static void db_close(Table* table);
static Pager* pager_open(const char* filename);
static void* get_page(Pager* pager, uint32_t page_num);
static void pager_flush(Pager* pager, uint32_t page_num);
static Cursor* table_start(Table* table);
static void cursor_advance(Cursor* cursor);
static uint32_t* leaf_node_num_cells(void* node);
static void* leaf_node_cell(void* node, uint32_t cell_num);
static uint32_t* leaf_node_key(void* node, uint32_t cell_num);
static void* leaf_node_value(void* node, uint32_t cell_num);
static void initialize_leaf_node(void* node);
static void initialize_internal_node(void* node);
static void leaf_node_insert(Cursor* cursor, uint32_t key, Row* value);
static void print_constants();
static Cursor* table_find(Table* table, uint32_t key);
static Cursor* leaf_node_find(Table* table, uint32_t page_num, uint32_t key);
static NodeType get_node_type(void* node);
static void set_node_type(void* node, NodeType type);
static void leaf_node_split_and_insert(Cursor* cursor, uint32_t key, Row* value);
static uint32_t get_unused_page_num(Pager* pager);
static void create_new_root(Table* table, uint32_t right_child_page_num);
static uint32_t* internal_node_num_keys(void* node);
static uint32_t* internal_node_right_child(void* node);
static uint32_t* internal_node_cell(void* node, uint32_t cell_num);
static uint32_t* internal_node_child(void* node, uint32_t child_num);
static uint32_t* internal_node_key(void* node, uint32_t key_num);
static uint32_t get_node_max_key(void* node);
static bool is_node_root(void* node);
static void set_node_root(void* node, bool is_root);
static void indent(uint32_t level);
static void print_tree(Pager* pager, uint32_t page_num, uint32_t indentation_level);
static Cursor* internal_node_find(Table* table, uint32_t page_num, uint32_t key);
static uint32_t internal_node_find_child(void* node, uint32_t key);
static uint32_t* leaf_node_next_leaf(void* node);
static uint32_t* node_parent(void* node);
static void update_internal_node_key(void* node, uint32_t old_key, uint32_t new_key);
static void print_tree(Pager* pager, uint32_t page_num, uint32_t indentation_level);
static void internal_node_insert(Table* table, uint32_t parent_page_num, uint32_t child_page_num);

static InputBuffer* new_input_buffer() {
	InputBuffer* input_buffer = (InputBuffer*)malloc(sizeof(InputBuffer));
	input_buffer->buffer = NULL;
	input_buffer->buffer_length = 0;
	input_buffer->input_length = 0;

	return input_buffer;
}

static void read_input(InputBuffer* input_buffer) {
	ssize_t bytes_read = getline(&(input_buffer->buffer), &(input_buffer->buffer_length), stdin);

	if (bytes_read <= 0) {
		printf("Error reading input\n");
		exit(EXIT_FAILURE);
	}

	// ignore traling newline
  	input_buffer->input_length = bytes_read - 1;
  	input_buffer->buffer[bytes_read - 1] = 0;
}

static void close_input_buffer(InputBuffer* input_buffer) {
	free(input_buffer->buffer);
	free(input_buffer);
}

static void print_prompt() { printf("db > "); }

static MetaCommandResult do_meta_command(InputBuffer* input_buffer, Table* table) {
	if (strcmp(input_buffer->buffer, ".exit") == 0) {
		db_close(table);
		exit(EXIT_SUCCESS);
	} else if (strcmp(input_buffer->buffer, ".btree") == 0) {
		printf("Tree:\n");
		print_tree(table->pager, 0, 0);
		return META_COMMAND_SUCCESS;
	} else if (strcmp(input_buffer->buffer, ".constants") == 0) {
		printf("Constants:\n");
		print_constants();
		return META_COMMAND_SUCCESS;
	} else {
		return META_COMMAND_UNRECOGNIZED_COMMAND;
	}
}

static PrepareResult prepare_statement(InputBuffer* input_buffer, Statement* statement) {
	if (strncmp(input_buffer->buffer, "insert", 6) == 0) {
		return prepare_insert(input_buffer, statement);
	}
	if (strcmp(input_buffer->buffer, "select") == 0) {
		statement->type = STATEMENT_SELECT;
		return PREPARE_SUCCESS;
	}

	return PREPARE_UNRECOGNIZED_STATEMENT;
}

static PrepareResult prepare_insert(InputBuffer* input_buffer, Statement* statement) {
	statement->type = STATEMENT_INSERT;
	
	char* keyword = strtok(input_buffer->buffer, " ");
	char* id_string = strtok(NULL, " ");
	char* username = strtok(NULL, " ");
	char* email = strtok(NULL, " ");

	if (id_string == NULL || username == NULL || email == NULL) {
		return PREPARE_SYNTAX_ERROR;
	}

	int id = atoi(id_string);
	if (id < 0) {
		return PREPARE_NEGATIVE_ID;
	}
	if (strlen(username) > COLUMN_USERNAME_SIZE) {
		return PREPARE_STRING_TOO_LONG;
	}
	if (strlen(email) > COLUMN_EMAIL_SIZE) {
		return PREPARE_STRING_TOO_LONG;
	}

	statement->row_to_insert.id = id;
	strcpy(statement->row_to_insert.username, username);
	strcpy(statement->row_to_insert.email, email);

	return PREPARE_SUCCESS;
}

static ExecuteResult execute_statement(Statement* statement, Table* table) {
	switch (statement->type) {
		case (STATEMENT_INSERT):
			return execute_insert(statement, table);
		case (STATEMENT_SELECT):
			return execute_select(statement, table);
	}
}

static ExecuteResult execute_insert(Statement* statement, Table* table) {
	void* node = get_page(table->pager, table->root_page_num);
	uint32_t num_cells = (*leaf_node_num_cells(node));

	Row* row_to_insert = &(statement->row_to_insert);
	uint32_t key_to_insert = row_to_insert->id;
	Cursor* cursor = table_find(table, key_to_insert);

	if (cursor->cell_num < num_cells) {
		uint32_t key_at_index = *leaf_node_key(node, cursor->cell_num);
		if (key_at_index == key_to_insert) {
			return EXECUTE_DUPLICATE_KEY;
		}
	}

	leaf_node_insert(cursor, row_to_insert->id, row_to_insert);

	free(cursor);

	return EXIT_SUCCESS;
}

static ExecuteResult execute_select(Statement* statement, Table* table) {
	Cursor* cursor = table_start(table);
	Row row;
	while (!(cursor->end_of_table)) {
		deserialize_row(cursor_value(cursor), &row);
		print_row(&row);
		cursor_advance(cursor);
	}

	free(cursor);

	return EXECUTE_SUCCESS;
}

static void print_row(Row* row) {
	printf("(%d, %s, %s)\n", row->id, row->username, row->email);
}

// copy the member value to the destination offset
static void serialize_row(Row* source, void* destination) {
	memcpy(destination + ID_OFFSET, &(source->id), ID_SIZE);
	strncpy(destination + USERNAME_OFFSET, source->username, USERNAME_SIZE);
	strncpy(destination + EMAIL_OFFSET, source->email, EMAIL_SIZE);
}

// copy the offset of the source to the offset of the member
static void deserialize_row(void* source, Row* destination) {
	memcpy(&(destination->id), source + ID_OFFSET, ID_SIZE);
	memcpy(&(destination->username), source + USERNAME_OFFSET, USERNAME_SIZE);
	memcpy(&(destination->email), source + EMAIL_OFFSET, EMAIL_SIZE);
}

// カーソルで記述された位置へのポインタを返す
static void* cursor_value(Cursor* cursor) {
	uint32_t page_num = cursor->page_num;
	void* page = get_page(cursor->table->pager, page_num);

	return leaf_node_value(page, cursor->cell_num);
}

static Table* db_open(const char* filename) {
	Pager* pager = pager_open(filename);
	printf("file_length: %d\n", pager->file_length);
	printf("row_size: %d\n", ROW_SIZE); // i think 291

	Table* table = (Table*)malloc(sizeof(Table));
	table->pager = pager;
	table->root_page_num = 0;

	// データベースファイルを新規作成する時、ページ0をリーフノードとして初期化する。
	if (pager->num_pages == 0) {
		void* root_node = get_page(pager, 0);
		initialize_leaf_node(root_node);
		set_node_root(root_node, true);
	}

	return table;
}

static void cursor_advance(Cursor* cursor) {
	uint32_t page_num = cursor->page_num;
	void* node = get_page(cursor->table->pager, page_num);

	cursor->cell_num += 1;
	if (cursor->cell_num >= *(leaf_node_num_cells(node))) {
		/* Advance to next leaf node */
		uint32_t next_page_num = *leaf_node_next_leaf(node);
    	if (next_page_num == 0) {
 			/* This was rightmost leaf */
 	     	cursor->end_of_table = true;
 	   	} else {
 	    	cursor->page_num = next_page_num;
 	    	cursor->cell_num = 0;
 	   }
	}
}

// ページキャッシュをディスクにフラッシュ
// データベースファイルを閉じる
// ページャとテーブルのデータ構造のためのメモリを解放
//
static void db_close(Table* table) {
	Pager* pager = table->pager;

	// ページキャッシュをディスクにフラッシュ
	for (uint32_t i = 0; i < pager->num_pages; i++) {
		if (pager->pages[i] == NULL) {
			continue;
		}
		pager_flush(pager, i);
		free(pager->pages[i]);
		pager->pages[i] = NULL;
	}

	// データベースファイルを閉じる
	int result = close(pager->file_descriptor);
	if (result == -1) {
		printf("Error closing db file.\n");
		exit(EXIT_FAILURE);
	}
	for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
		void* page = pager->pages[i];
		if (page) {
			free(page);
			pager->pages[i] = NULL;
		}
	}
	free(pager);
	free(table);
}

// データベースのサイズを保存し、キャッシュを削除する
//
static Pager* pager_open(const char* filename) {
	int fd = open(filename,
				O_RDWR | O_CREAT,
				S_IWUSR | S_IRUSR
			);
	if (fd == -1) {
		printf("Unable to open file\n");
		exit(EXIT_FAILURE);
	}

	// fdの終わりまでポインタを移動する
	off_t file_length = lseek(fd, 0, SEEK_END);

	Pager* pager = malloc(sizeof(Pager));
	pager->file_descriptor = fd;
	pager->file_length = file_length;
	pager->num_pages = (file_length / PAGE_SIZE);

	if (file_length % PAGE_SIZE != 0) {
		printf("DB file is not a whole number of pages. Corrupt file. \n");
		exit(EXIT_FAILURE);
	}

	// pagesのキャッシュを削除
	for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
		pager->pages[i] = NULL;
	}

	return pager;
}

static void* get_page(Pager* pager, uint32_t page_num) {
	if (page_num > TABLE_MAX_PAGES) {
		printf("Tried to fetch page number out of bounds. %d > %d\n",
				page_num, TABLE_MAX_PAGES);
		exit(EXIT_FAILURE);
	}

	// キャッシュミス対応。ページサイズを確保する。
	if (pager->pages[page_num] == NULL) {
		void* page = malloc(PAGE_SIZE);
		uint32_t num_pages = pager->file_length / PAGE_SIZE;

		// PAGE_SIZEに収まり切らない時に、部分的にキャッシュを保存させる必要がある
		if (pager->file_length % PAGE_SIZE) {
			num_pages += 1;
		}

		// ファイル記述子のポインタをページ*4096分移動して、書き込み開始位置まで移動する
		// 移動してから、PAGE_SIZE分取得する
		if (page_num <= num_pages) {
			lseek(pager->file_descriptor, page_num * PAGE_SIZE, SEEK_SET);
			ssize_t bytes_read = read(pager->file_descriptor, page, PAGE_SIZE);
			if (bytes_read == -1) {
				printf("Error reading file: %d\n", errno);
				exit(EXIT_FAILURE);
			}
		}

		pager->pages[page_num] = page;

		if (page_num >= pager->num_pages) {
			pager->num_pages = page_num + 1;
		}
	}

	return pager->pages[page_num];
}

// ページキャッシュをディスクにフラッシュ
static void pager_flush(Pager* pager, uint32_t page_num) {
	if (pager->pages[page_num] == NULL) {
		printf("Tried to flush null page\n");
		exit(EXIT_FAILURE);
	}

	off_t offset = lseek(pager->file_descriptor, page_num * PAGE_SIZE, SEEK_SET);
	
	if (offset == -1) {
		printf("Error seeking: %d\n", errno);
		exit(EXIT_FAILURE);
	}

	ssize_t bytes_written = write(pager->file_descriptor,
				pager->pages[page_num], PAGE_SIZE);
	
	if (bytes_written == -1) {
		printf("Error writing: %d\n", errno);
		exit(EXIT_FAILURE);
	}
}

static Cursor* table_start(Table* table) {
	Cursor* cursor = table_find(table, 0);

	void* node = get_page(table->pager, cursor->page_num);
	uint32_t num_cells = *leaf_node_num_cells(node);
	cursor->end_of_table = (num_cells == 0);

	return cursor;
}

// リーフノードのセルの位置を返す
//
static uint32_t* leaf_node_num_cells(void* node) {
	return node + LEAF_NODE_NUM_CELLS_OFFSET;
}

// leaf nodeのセルの位置を返す
//
static void* leaf_node_cell(void* node, uint32_t cell_num) {
	return node + LEAF_NODE_HEADER_SIZE + cell_num * LEAF_NODE_CELL_SIZE;
}

// leaf nodeのkeyを返す
//
static uint32_t* leaf_node_key(void* node, uint32_t cell_num) {
	return leaf_node_cell(node, cell_num);
}

// leaf nodeのkeyのvalueを返す
//
static void* leaf_node_value(void* node, uint32_t cell_num) {
	return leaf_node_cell(node, cell_num) + LEAF_NODE_KEY_SIZE;
}

static void initialize_leaf_node(void* node) {
	set_node_type(node, NODE_LEAF);
	set_node_root(node, false);
	*leaf_node_num_cells(node) = 0;
	*leaf_node_next_leaf(node) = 0;
}

static void initialize_internal_node(void* node) {
	set_node_type(node, NODE_INTERNAL);
	set_node_root(node, false);
	*internal_node_num_keys(node) = 0;
}

// キーと値のペアをリーフノードに挿入するための関数を作成します。
// ペアを挿入する位置を表すために、引数にカーソルをとる
//
static void leaf_node_insert(Cursor* cursor, uint32_t key, Row* value) {
	void* node = get_page(cursor->table->pager, cursor->page_num);

	uint32_t num_cells = *leaf_node_num_cells(node);
	if (num_cells >= LEAF_NODE_MAX_CELLS) {
		// Node full
		leaf_node_split_and_insert(cursor, key, value);
		return;
	}

	// 新しいセルのためのスペースを確保する
	if (cursor->cell_num < num_cells) {
		for (uint32_t i = num_cells; i > cursor->cell_num; i--) {
			memcpy(leaf_node_cell(node, i), leaf_node_cell(node, i - 1),
					LEAF_NODE_CELL_SIZE);
		}
	}

	*(leaf_node_num_cells(node)) += 1;
	*(leaf_node_key(node, cursor->cell_num)) = key;
	serialize_row(value, leaf_node_value(node, cursor->cell_num));
}

static void print_constants() {
  printf("ROW_SIZE: %d\n", ROW_SIZE);
  printf("COMMON_NODE_HEADER_SIZE: %d\n", COMMON_NODE_HEADER_SIZE);
  printf("LEAF_NODE_HEADER_SIZE: %d\n", LEAF_NODE_HEADER_SIZE);
  printf("LEAF_NODE_CELL_SIZE: %d\n", LEAF_NODE_CELL_SIZE);
  printf("LEAF_NODE_SPACE_FOR_CELLS: %d\n", LEAF_NODE_SPACE_FOR_CELLS);
  printf("LEAF_NODE_MAX_CELLS: %d\n", LEAF_NODE_MAX_CELLS);
}

// キーの位置を返す
// キーが存在しない場合、キーが挿入されるべき位置を返す
static Cursor* table_find(Table* table, uint32_t key) {
	uint32_t root_page_num = table->root_page_num;
	void* root_node = get_page(table->pager, root_page_num);

	if (get_node_type(root_node) == NODE_LEAF) {
		return leaf_node_find(table, root_page_num, key);
	} else {
		return internal_node_find(table, root_page_num, key);
	}
}

// 二分探索でleaf nodeを探索
static Cursor* leaf_node_find(Table* table, uint32_t page_num, uint32_t key) {
	void* node = get_page(table->pager, page_num);
	uint32_t num_cells = *leaf_node_num_cells(node);

	Cursor* cursor = malloc(sizeof(Cursor));
	cursor->table = table;
	cursor->page_num = page_num;

	// Binary search
	// キーの位置か新しいキーを挿入するために移動させる必要がある別のキーの位置or最後のキーの1つ前の位置
	uint32_t min_index = 0;
	uint32_t one_past_max_index = num_cells;
	while (one_past_max_index != min_index) {
		uint32_t index = (min_index + one_past_max_index) / 2;
		uint32_t key_at_index = *leaf_node_key(node, index);
		if (key == key_at_index) {
			cursor->cell_num = index;
			return cursor;
		}
		if (key < key_at_index) {
			one_past_max_index = index;
		} else {
			min_index = index + 1;
		}
	}
	
	cursor->cell_num = min_index;
	return cursor;
}

static NodeType get_node_type(void* node) {
	uint8_t value = *((uint8_t*)(node + NODE_TYPE_OFFSET));
	return (NodeType)value;
}

static void set_node_type(void* node, NodeType type) {
	uint8_t value = type;
	*((uint8_t*)(node + NODE_TYPE_OFFSET)) = value;
}

static void leaf_node_split_and_insert(Cursor* cursor, uint32_t key, Row* value) {
	// 新しいノードを作成し、セルの半分を移動
	// 2つのノードのうち1つに新しい値を挿入
	// 親を更新するか、新しい親を作成
	void* old_node = get_page(cursor->table->pager, cursor->page_num);
	uint32_t old_max = get_node_max_key(old_node);
	uint32_t new_page_num = get_unused_page_num(cursor->table->pager);
	void* new_node = get_page(cursor->table->pager, new_page_num);
	initialize_leaf_node(new_node);
	*node_parent(new_node) = *node_parent(old_node);
	// リーフノードを分割するたびに、兄弟ポインターを更新
	*leaf_node_next_leaf(new_node) = *leaf_node_next_leaf(old_node);
	*leaf_node_next_leaf(old_node) = new_page_num;
	
	// すべての既存キーと新しいキーを旧ノード（左）と新ノード（右）に均等に分割
	// 旧ノード（左）と新ノード（右）の間で均等に分割する必要
	// 右から順に、各キーを正しい位置に移動させる
	for (int32_t i = LEAF_NODE_MAX_CELLS; i >= 0; i--) {
	  void* destination_node;
	  if (i >= LEAF_NODE_LEFT_SPLIT_COUNT) {
	    destination_node = new_node;
	  } else {
	    destination_node = old_node;
	  }
	  uint32_t index_within_node = i % LEAF_NODE_LEFT_SPLIT_COUNT;
	  void* destination = leaf_node_cell(destination_node, index_within_node);
	
	  if (i == cursor->cell_num) {
		serialize_row(value, leaf_node_value(destination_node, index_within_node));
		*leaf_node_key(destination_node, index_within_node) = key;
	  } else if (i > cursor->cell_num) {
	    memcpy(destination, leaf_node_cell(old_node, i - 1), LEAF_NODE_CELL_SIZE);
	  } else {
	    memcpy(destination, leaf_node_cell(old_node, i), LEAF_NODE_CELL_SIZE);
	  }
	}
	
	// 各ノードのヘッダーのセル数を更新
	*(leaf_node_num_cells(old_node)) = LEAF_NODE_LEFT_SPLIT_COUNT;
	*(leaf_node_num_cells(new_node)) = LEAF_NODE_RIGHT_SPLIT_COUNT;
	
	// ノードの親を更新
	// 元のノードがルートであった場合、そのノードには親がない。
	// この場合、新しいルート・ノードを作成して、親として機能させる
	if (is_node_root(old_node)) {
	  return create_new_root(cursor->table, new_page_num);
	} else {
		uint32_t parent_page_num = *node_parent(old_node);
		uint32_t new_max = get_node_max_key(old_node);
		void* parent = get_page(cursor->table->pager, parent_page_num);
		
		update_internal_node_key(parent, old_max, new_max);
		internal_node_insert(cursor->table, parent_page_num, new_page_num);
		return;
	}
}

static uint32_t get_unused_page_num(Pager* pager) {
	return pager->num_pages;
}

// ルートの分割を処理する。
// 古いルートは新しいページにコピーされ、左の子になる。
// 右の子のアドレスが渡される。
// 新しいルート・ノードが含まれるようにルート・ページを再初期化する。
// 新しいルート・ノードが2つの子を指す。
//
static void create_new_root(Table* table, uint32_t right_child_page_num) {
	void* root = get_page(table->pager, table->root_page_num);
	void* right_child = get_page(table->pager, right_child_page_num);
	uint32_t left_child_page_num = get_unused_page_num(table->pager);
	void* left_child = get_page(table->pager, left_child_page_num);

	// 左の子のデータをrootにコピー
	memcpy(left_child, root, PAGE_SIZE);
	set_node_root(left_child, false);

	// ルートページを新しい内部ノードとして初期化し、2つの子ノードを作成
	initialize_internal_node(root);
	set_node_root(root, true);
	*internal_node_num_keys(root) = 1;
	*internal_node_child(root, 0) = left_child_page_num;
	uint32_t left_child_max_key = get_node_max_key(left_child);
	*internal_node_key(root, 0) = left_child_max_key;
	*internal_node_right_child(root) = right_child_page_num;
	*node_parent(left_child) = table->root_page_num;
	*node_parent(right_child) = table->root_page_num;
}

static uint32_t* internal_node_num_keys(void* node) {
	return node + INTERNAL_NODE_NUM_KEYS_OFFSET;
}

static uint32_t* internal_node_right_child(void* node) {
	return node + INTERNAL_NODE_RIGHT_CHILD_OFFSET;
}

static uint32_t* internal_node_cell(void* node, uint32_t cell_num) {
	return node + INTERNAL_NODE_HEADER_SIZE + cell_num * INTERNAL_NODE_CELL_SIZE;
}

static uint32_t* internal_node_child(void* node, uint32_t child_num) {
	uint32_t num_keys = *internal_node_num_keys(node);
	if (child_num > num_keys) {
		printf("Tried to access child_num %d > num_keys %d\n", child_num, num_keys);
		exit(EXIT_FAILURE);
	} else if (child_num == num_keys) {
		return internal_node_right_child(node);
	} else {
		return internal_node_cell(node, child_num);
	}
}

static uint32_t* internal_node_key(void* node, uint32_t key_num) {
	return (void*)internal_node_cell(node, key_num) + INTERNAL_NODE_CHILD_SIZE;
}

// 内部ノードの場合、最大キーは常にその右側のキー
// 葉ノードでは、最大インデックスのキー
//
static uint32_t get_node_max_key(void* node) {
	switch (get_node_type(node)) {
		case NODE_INTERNAL:
			return *internal_node_key(node, *internal_node_num_keys(node) - 1);
		case NODE_LEAF:
			return *leaf_node_key(node, *leaf_node_num_cells(node) - 1);
	}
}

static bool is_node_root(void* node) {
	uint8_t value = *((uint8_t*)(node + IS_ROOT_OFFSET));
	return (bool)value;
}

static void set_node_root(void* node, bool is_root) {
	uint8_t value = is_root;
	*((uint8_t*)(node + IS_ROOT_OFFSET)) = value;
}

static void indent(uint32_t level) {
	for (uint32_t i = 0; i < level; i++) {
		printf("  ");
	}
}

static void print_tree(Pager* pager, uint32_t page_num, uint32_t indentation_level) {
  void* node = get_page(pager, page_num);
  uint32_t num_keys, child;

  switch (get_node_type(node)) {
    case (NODE_LEAF):
      num_keys = *leaf_node_num_cells(node);
      indent(indentation_level);
      printf("- leaf (size %d)\n", num_keys);
      for (uint32_t i = 0; i < num_keys; i++) {
        indent(indentation_level + 1);
        printf("- %d\n", *leaf_node_key(node, i));
      }
      break;
    case (NODE_INTERNAL):
      num_keys = *internal_node_num_keys(node);
      indent(indentation_level);
      printf("- internal (size %d)\n", num_keys);
      for (uint32_t i = 0; i < num_keys; i++) {
        child = *internal_node_child(node, i);
        print_tree(pager, child, indentation_level + 1);

        indent(indentation_level + 1);
        printf("- key %d\n", *internal_node_key(node, i));
      }
      child = *internal_node_right_child(node);
      print_tree(pager, child, indentation_level + 1);
      break;
  }
}

static uint32_t internal_node_find_child(void* node, uint32_t key) {
	uint32_t num_keys = *internal_node_num_keys(node);

	// Binary Serach
	uint32_t min_index = 0;
	uint32_t max_index = num_keys;

	while(min_index != max_index) {
		uint32_t index = (min_index / max_index) / 2;
		uint32_t key_to_right = *internal_node_key(node, index);
		if (key_to_right >= key) {
			max_index = index;
		} else {
			min_index = index + 1;
		}
	}
	
	return min_index;
}

static Cursor* internal_node_find(Table* table, uint32_t page_num, uint32_t key) {
  void* node = get_page(table->pager, page_num);

  uint32_t child_index = internal_node_find_child(node, key);
  uint32_t child_num = *internal_node_child(node, child_index);
  void* child = get_page(table->pager, child_num);
  switch (get_node_type(child)) {
    case NODE_LEAF:
      return leaf_node_find(table, child_num, key);
    case NODE_INTERNAL:
      return internal_node_find(table, child_num, key);
  }
}

static uint32_t* leaf_node_next_leaf(void* node) {
	return node + LEAF_NODE_NEXT_LEAF_OFFSET;
}

static uint32_t* node_parent(void* node) {
	return node + PARENT_POINTER_OFFSET;
}

static void update_internal_node_key(void* node, uint32_t old_key, uint32_t new_key) {
	uint32_t old_child_index = internal_node_find_child(node, old_key);
	*internal_node_key(node, old_child_index) = new_key;
}

static void internal_node_insert(Table* table, uint32_t parent_page_num,
                          uint32_t child_page_num) {
  /*
  Add a new child/key pair to parent that corresponds to child
  */

  void* parent = get_page(table->pager, parent_page_num);
  void* child = get_page(table->pager, child_page_num);
  uint32_t child_max_key = get_node_max_key(child);
  uint32_t index = internal_node_find_child(parent, child_max_key);

  uint32_t original_num_keys = *internal_node_num_keys(parent);
  *internal_node_num_keys(parent) = original_num_keys + 1;

  if (original_num_keys >= INTERNAL_NODE_MAX_CELLS) {
    printf("Need to implement splitting internal node\n");
    exit(EXIT_FAILURE);
  }

  uint32_t right_child_page_num = *internal_node_right_child(parent);
  void* right_child = get_page(table->pager, right_child_page_num);

  if (child_max_key > get_node_max_key(right_child)) {
    /* Replace right child */
    *internal_node_child(parent, original_num_keys) = right_child_page_num;
    *internal_node_key(parent, original_num_keys) =
        get_node_max_key(right_child);
    *internal_node_right_child(parent) = child_page_num;
  } else {
    /* Make room for the new cell */
    for (uint32_t i = original_num_keys; i > index; i--) {
      void* destination = internal_node_cell(parent, i);
      void* source = internal_node_cell(parent, i - 1);
      memcpy(destination, source, INTERNAL_NODE_CELL_SIZE);
    }
    *internal_node_child(parent, index) = child_page_num;
    *internal_node_key(parent, index) = child_max_key;
  }
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
		printf("Must supply a database filename.\n");
		exit(EXIT_FAILURE);
	}

	char* filename = argv[1];
	Table* table = db_open(filename);

	InputBuffer* input_buffer = new_input_buffer();
	while(true) {
		print_prompt();
		read_input(input_buffer);

		if (input_buffer->buffer[0] == '.') {
			switch (do_meta_command(input_buffer, table)) {
				case (META_COMMAND_SUCCESS):
					continue;
				case (META_COMMAND_UNRECOGNIZED_COMMAND):
					printf("Unrecognized command '%s'\n", input_buffer->buffer);
					continue;
			}
		}

		Statement statement;
		switch (prepare_statement(input_buffer, &statement)) {
			case (PREPARE_SUCCESS):
				break;
			case (PREPARE_STRING_TOO_LONG):
				printf("String is too long.\n");
				continue;
			case (PREPARE_NEGATIVE_ID):
				printf("ID must be positive.\n");
				continue;
			case (PREPARE_SYNTAX_ERROR):
				printf("Syntax error, could not parse statement.\n");
				continue;
			case (PREPARE_UNRECOGNIZED_STATEMENT):
				printf("Unrecognized keyword at start of '%s'. \n", input_buffer->buffer);
				continue;
		}
		switch (execute_statement(&statement, table)) {
			case (EXECUTE_SUCCESS):
				printf("Executed.\n");
				break;
			case (EXECUTE_DUPLICATE_KEY):
				printf("Error: Duplicate key.\n");
				break;
			case (EXECUTE_TABLE_FULL):
				printf("Error: Table full.\n");
				break;
		}
	}
}
