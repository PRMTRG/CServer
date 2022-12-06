#define POST_NAME_MAXLEN 64
#define POST_TIMESTAMP_MAXLEN 64
#define POST_FILENAME_MAXLEN 64
#define POST_COMMENT_MAXLEN 2048
#define POST_FILE_MAXSIZE 1024 * 1024 * 3
#define THREAD_SUBJECT_MAXLEN 64

typedef struct {
    long post_id;
    long thread_id;
    char name[POST_NAME_MAXLEN];
    char timestamp[POST_TIMESTAMP_MAXLEN];
    char filename[POST_FILENAME_MAXLEN];
    char *comment;
    int hidden;
} post_t;

typedef struct {
    long thread_id;
    char subject[THREAD_SUBJECT_MAXLEN];
    post_t *posts;
    long nposts;
    long posts_allocated;
    int no_bump;
    int valid_page_cache;
} thread_t;

int posts_get_by_thread_id(long thread_id, post_t **posts, long *nposts);
void threads_get(thread_t **threads, long *nthreads);

int post_create(long thread_id, post_t *post);
int thread_create(post_t *post, const char *subject);

void delete_post_or_thread(long post_id);

void forum_init(void);
