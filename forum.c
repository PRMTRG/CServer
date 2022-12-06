#define _POSIX_C_SOURCE 200112L
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "utils.h"
#include "forum.h"

#define MAX_THREADS 1000
#define THREAD_BUMP_LIMIT 200

#define THREAD_CACHE_RESIZE_INC 100
#define POST_CACHE_RESIZE_INC 1000

static thread_t *threads = NULL;
static long nthreads = 0;
static long threads_allocated = 0;

static long next_post_id = 2137;

static long
get_next_post_id(void)
{
    return next_post_id++;
}

static void
get_timestamp_string(char *buf, int bufsize)
{
    time_t t = time(NULL);
    struct tm *timeptr = localtime(&t);
    if (!timeptr) {
        fprintf(stderr, "get_timestamp_string: localtime() failed.\n");
        exit(1);
    }
    snprintf(buf, bufsize, "%04d-%02d-%02d %02d:%02d:%02d", timeptr->tm_year + 1900, timeptr->tm_mon + 1,
            timeptr->tm_mday, timeptr->tm_hour, timeptr->tm_min, timeptr->tm_sec);
}

static int
validate_post(post_t *post, int post_is_op, const char *subject)
{
    if (!post->comment || !*post->comment) {
        fprintf(stderr, "validate_post: Missing comment.\n");
        return 1;
    }
    if (post_is_op && !*post->filename) {
        fprintf(stderr, "validate_post: Missing filename.\n");
        return 1;
    }
    if (post_is_op && (!subject || !*subject)) {
        fprintf(stderr, "validate_post: Missing subject.\n");
        return 1;
    }

    int clen = strlen(post->comment);
    if (clen + 1 > POST_COMMENT_MAXLEN) {
        fprintf(stderr, "validate_post: Comment too large.\n");
        return 1;
    }

    return 0;
}

int
posts_get_by_thread_id(long thread_id, post_t **posts, long *nposts)
{
    thread_t *thread = NULL;
    for (long i = 0; i < nthreads; i++) {
        if (threads[i].thread_id == thread_id) {
            thread = &threads[i];
            break;
        }
    }
    if (!thread) {
        fprintf(stderr, "posts_get_by_thread_id: Thread not found.\n");
        return 1;
    }
    *posts = thread->posts;
    *nposts = thread->nposts;
    return 0;
}

static post_t *post_get_by_id(long post_id);
static void post_fields_delete(post_t *post);
static void post_set_hidden_by_id(long post_id);
static void thread_delete_by_pos(long pos);

static post_t *
post_get_by_id(long post_id)
{
    for (long i = 0; i < nthreads; i++) {
        thread_t *thread = &threads[i];
        for (long j = 0; j < thread->nposts; j++) {
            post_t *post = &thread->posts[j];
            if (post->post_id == post_id) {
                return post;
            }
        }
    }
    return NULL;
}

static void
post_fields_delete(post_t *post)
{
    free(post->comment);

    if (*post->filename && strcmp(post->filename, PLACEHOLDER_IMAGE_FILENAME) != 0) {
        const char olddir[] = "uploads/";
        const char newdir[] = "uploads/deleted/";

        int fnlen = strlen(post->filename);

        if (sizeof(newdir) + fnlen > 256) {
            fprintf(stderr, "post_fields_delete: Buffer too small.\n");
            exit(1);
        }
        
        char oldpath[256];
        memcpy(oldpath, olddir, sizeof(olddir) - 1);
        memcpy(&oldpath[sizeof(olddir) - 1], post->filename, fnlen + 1);

        char newpath[256];
        memcpy(newpath, newdir, sizeof(newdir) - 1);
        memcpy(&newpath[sizeof(newdir) - 1], post->filename, fnlen + 1);

        int ret = rename(oldpath, newpath);
        if (ret != 0) {
            perror("post_fields_delete: rename()");
        }
    }
}

static void
post_set_hidden_by_id(long post_id)
{
    post_t *post = post_get_by_id(post_id);
    if (!post) {
        fprintf(stderr, "post_set_hidden_by_id: Post not found.\n");
        return;
    }
    post->hidden = 1;
}

int
post_create(long thread_id, post_t *p)
{
    thread_t *thread = NULL;
    int thread_is_first = 0;
    for (long i = 0; i < nthreads; i++) {
        if (threads[i].thread_id == thread_id) {
            thread = &threads[i];
            if (i == 0)
                thread_is_first = 1;
            break;
        }
    }
    if (!thread) {
        fprintf(stderr, "post_create: Thread not found.\n");
        return 1;
    }

    int post_is_op = (thread->nposts == 0) ? 1 : 0;
    /* When creating a new thread validate_post() must be run in thread_create(). */
    if (!post_is_op) {
        int ret = validate_post(p, 0, NULL);
        if (ret != 0) {
            fprintf(stderr, "post_create: Failed to validate post.\n");
            return 1;
        }
    }

    if (thread->nposts + 1 > thread->posts_allocated) {
        post_t *tmp = realloc(thread->posts, (thread->posts_allocated + POST_CACHE_RESIZE_INC) * sizeof(post_t));
        if (!tmp) {
            fprintf(stderr, "post_create: realloc() failed.\n");
            exit(1);
        }
        memset(&tmp[thread->posts_allocated], 0, POST_CACHE_RESIZE_INC * sizeof(post_t));
        thread->posts = tmp;
        thread->posts_allocated += POST_CACHE_RESIZE_INC;
    }
    post_t *post = &thread->posts[thread->nposts];
    thread->nposts++;

    if (thread->nposts > THREAD_BUMP_LIMIT) {
        thread->no_bump = 1;
    }


    /* post_id */
    if (post_is_op) {
        post->post_id = thread->thread_id;
        p->post_id = post->post_id;
    } else {
        post->post_id = get_next_post_id();
        p->post_id = post->post_id;
    }

    /* thread_id */
    post->thread_id = thread->thread_id;
    p->thread_id = post->thread_id;

    /* name */
    if (*p->name) {
        memcpy(post->name, p->name, POST_NAME_MAXLEN);
    } else {
        static const char default_name[POST_NAME_MAXLEN] = "Anonymous";
        memcpy(post->name, default_name, POST_NAME_MAXLEN);
    }

    /* comment */
    post->comment = p->comment;

    /* filename */
    if (*p->filename) {
        memcpy(post->filename, p->filename, POST_FILENAME_MAXLEN);
    }

    /* timestamp */
    get_timestamp_string(post->timestamp, POST_TIMESTAMP_MAXLEN);
    
    // TODO: Move thread to front.
    if (!thread->no_bump) {
        (void) thread_is_first;
    }

    return 0;
}

static void
thread_delete_by_pos(long pos)
{
    thread_t *thread = &threads[pos];
    for (long i = 0; i < thread->nposts; i++) {
        post_fields_delete(&thread->posts[i]);
    }
    free(thread->posts);

    if (nthreads > pos + 1) {
        memmove(&threads[pos], &threads[pos + 1], (nthreads - pos - 1) * sizeof(thread_t));
    }
    nthreads--;
}

void
delete_post_or_thread(long post_id)
{
    for (long i = 0; i < nthreads; i++) {
        if (threads[i].thread_id == post_id) {
            thread_delete_by_pos(i);
            return;
        }
    }

    post_set_hidden_by_id(post_id);
}

int
thread_create(post_t *p, const char *subject)
{
    int ret = validate_post(p, 1, subject);
    if (ret != 0) {
        fprintf(stderr, "thread_create: Failed to validate post.\n");
        return 1;
    }

    if (nthreads + 1 > threads_allocated) {
        thread_t *tmp = realloc(threads, (threads_allocated + THREAD_CACHE_RESIZE_INC) * sizeof(thread_t));
        if (!tmp) {
            fprintf(stderr, "thread_create: realloc() failed.\n");
            exit(1);
        }
        threads = tmp;
        threads_allocated += THREAD_CACHE_RESIZE_INC;
    }
    memmove(&threads[1], &threads[0], nthreads * sizeof(thread_t));
    memset(&threads[0], 0, sizeof(thread_t));
    nthreads++;
    thread_t *thread = &threads[0];
    long id = get_next_post_id();
    thread->thread_id = id;
    memcpy(&thread->subject, subject, THREAD_SUBJECT_MAXLEN);
    if (post_create(id, p) != 0) {
        fprintf(stderr, "thread_create: post_create() failed. (how?)\n");
        exit(1);
    }

    if (nthreads > MAX_THREADS) {
        thread_delete_by_pos(nthreads - 1);
    }

    return 0;
}

void
threads_get(thread_t **t, long *nt)
{
    *t = threads;
    *nt = nthreads;
}

static const char *sample_comments[] = {
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit.<br><br>Praesent interdum vitae ante non accumsan.<br>Donec eu pretium ipsum. Donec sit amet urna nisl. Class aptent taciti sociosqu ad litora torquent per conubia nostra, per inceptos himenaeos.<br>Proin vulputate ligula interdum nisi euismod sagittis.<br>Sed porttitor purus at urna ultrices, id ultricies lacus porttitor.<br><br>Proin nec justo vel lorem mattis feugiat.",
    "Phasellus aliquam molestie maximus. Mauris porttitor aliquam velit a tristique. Nulla at enim efficitur, gravida quam sed, lobortis metus.<br><br>Morbi iaculis sem et mauris rhoncus, in mattis ipsum dapibus. Sed at placerat nisl. Sed tincidunt rhoncus luctus. Vivamus lobortis maximus arcu. Fusce ac gravida ipsum. Donec a leo vitae augue convallis malesuada. Suspendisse pellentesque tincidunt lectus, sit amet eleifend nisl porttitor nec. Fusce tempus condimentum est euismod sagittis.",
    "Fusce eleifend luctus elit.<br>Donec massa lectus, porta sed pellentesque vel, dignissim sed dui.<br>Quisque volutpat, leo non viverra semper, quam augue iaculis risus, eget consectetur ligula velit sit amet lectus. Integer a pellentesque arcu, egestas efficitur ligula. Donec sit amet lobortis risus. Vivamus molestie sapien sed suscipit aliquet. Pellentesque habitant morbi tristique senectus et netus et malesuada fames ac turpis egestas. Donec quis mi ut felis viverra consequat.<br>Pellentesque laoreet, augue id auctor porttitor, urna ante volutpat dolor, et condimentum ex magna dictum nunc. Sed at maximus risus, at tincidunt elit.<br>Morbi sit amet sapien mattis, varius lorem eget, rutrum risus.<br>Donec maximus nulla ante, ut porta tellus sollicitudin sit amet. Aenean at massa ipsum. Ut in tincidunt libero. Morbi iaculis auctor dolor, non ullamcorper sem vehicula vel. Aenean lacinia erat sed odio tristique sollicitudin. In ut aliquet turpis. Nulla ipsum tellus, sollicitudin a pulvinar at, cursus ac felis. Proin non elit vel libero semper molestie. Ut fermentum blandit elit a tincidunt. In mattis libero at hendrerit luctus.",
    "Sed eget arcu nunc.<br>Nam sed rhoncus velit, in hendrerit nulla.<br>Vivamus dapibus eleifend libero, vitae efficitur elit varius ut. Duis ultrices lectus eget ullamcorper euismod.<br>Vivamus sodales nec quam a tempus.", 
};

static int
create_sample_post(long thread_id)
{
    post_t post = {0};

    int c = rand() % (sizeof(sample_comments) / sizeof(char *));
    int clen = strlen(sample_comments[c]);
    char *comment = malloc(clen + 1);
    if (!comment) {
        fprintf(stderr, "create_sample_post: malloc() failed.\n");
        exit(1);
    }
    memcpy(comment, sample_comments[c], clen + 1);
    post.comment = comment;

    static const char filename[POST_FILENAME_MAXLEN] = "placeholder.png";
    if (rand() % 2) {
        memcpy(post.filename, filename, POST_FILENAME_MAXLEN);
    }

    int ret = post_create(thread_id, &post);
    if (ret != 0) {
        fprintf(stderr, "create_sample_post: post_create() failed.\n");
        exit(1);
    }

    return 0;
}

static int
create_sample_thread(int np)
{
    post_t op = {0};

    int c = rand() % (sizeof(sample_comments) / sizeof(char *));
    int clen = strlen(sample_comments[c]);
    char *comment = malloc(clen + 1);
    if (!comment) {
        fprintf(stderr, "create_sample_post: malloc() failed.\n");
        exit(1);
    }
    memcpy(comment, sample_comments[c], clen + 1);
    op.comment = comment;

    static const char filename[POST_FILENAME_MAXLEN] = "placeholder.png";
    memcpy(op.filename, filename, POST_FILENAME_MAXLEN);

    static const char subject[] = "Green Is My Pepper";

    int ret = thread_create(&op, subject);
    if (ret != 0) {
        fprintf(stderr, "create_random_thread: thread_create() failed.\n");
        exit(1);
    }
    long thread_id = op.post_id;

    for (int i = 0; i < np; i++) {
        int ret = create_sample_post(thread_id);
        if (ret != 0) {
            fprintf(stderr, "create_random_thread: create_random_post() failed.\n");
            exit(1);
        }
    }

    return 0;
}

void
forum_init(void)
{
    threads = calloc(THREAD_CACHE_RESIZE_INC, sizeof(thread_t));
    threads_allocated = THREAD_CACHE_RESIZE_INC;

#if 1
    for (int i = 0; i < 20; i++) {
        create_sample_thread(100);
    }
#endif

}
