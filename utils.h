int load_file_to_new_buffer(const char *filename, char **f, long *fs);
void parse_long(long **l, char *str);
void append_to_buffer_realloc_if_necessary(char **buf, long *bufpos, long *bufs, char *str, long len);
void string_to_lowercase(char *str);
char *copy_string(const char *str);
void gen_filename(char *buf, const int maxlen, const char *ext, const int extlen);
void save_file(const char *buf, const long bufs, const char *directory, const char *filename);
void start_timer(void);
void stop_timer(void);
