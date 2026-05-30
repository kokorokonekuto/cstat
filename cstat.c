#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <err.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PROC_STAT          "/proc/stat"
#define PROC_MEMINFO       "/proc/meminfo"
#define COLOR_GREEN        "\x1b[1;92m"
#define COLOR_LIGHT_RED    "\x1b[1;38;5;203m"
#define COLOR_LIGHT_PINK   COLOR_GREEN
#define COLOR_INTENSE_RED  "\x1b[1;38;5;196m"
#define COLOR_WHITE        "\x1b[1;97m"
#define COLOR_END          "\x1b[0m"
#define CLEAR_RESET_CURSOR "\x1b[2J\x1b[1;1H"

/* Maximum length of the bar. */
#define BAR_MAX_LEN    32

/* Maximum length of the buffer used by pretty_bytes(). */
#undef BUFLEN
#define BUFLEN         40
	
#define DEFAULT_PRETTY_UNIT    (1000.0)

#define MAX_LISTEN_QUEUE       (4)

#ifndef MIN
# define MIN(x, y)     (((x)<(y)) ? (x) : (y))
#endif

#ifndef ARRAY_SIZE
# define ARRAY_SIZE(x)  (sizeof(x)/sizeof(*x))
#endif

/* Maximum number of cores (including HT) can be indexed. */
#define MAX_CPU_CORES       (512*2+1)

struct cpu {
	uint64_t user;
	uint64_t nice;
	uint64_t system;
	uint64_t idle;
	int64_t iowait;
	uint64_t irq;
	uint64_t softirq;
	uint64_t steal;
};

struct cpus {
        struct cpu *cpus[MAX_CPU_CORES];
	uint32_t idx;
};

struct meminfo {
	uint64_t mem_total;
	uint64_t mem_free; /* maybe MemAvailable? */
	uint64_t mem_used;
        uint64_t swap_total;
	uint64_t swap_used;
	uint64_t swap_free;
};

static char *read_from_file(const char *path)
{
	int fd;
	char *p, buf[1024];
	ssize_t read_bytes;
	size_t nallocs;

	if ((fd = open(path, O_RDONLY)) == -1)
		err(1, "open");
	if ((p = malloc(sizeof(char))) == NULL)
		err(1, "calloc");

	memset(buf, '\0', sizeof(buf));
        nallocs = 1;
	while ((read_bytes = read(fd, buf, sizeof(buf) - 1)) > 0) {
		if ((p = realloc(p, nallocs+read_bytes+1)) == NULL)
			err(1, "realloc");
		memcpy(p+nallocs-1, buf, read_bytes);
		nallocs += read_bytes;
	}
	close(fd);
	p[nallocs-1] = '\0';
	return (p);
}

static void cpus_do_init(struct cpus *cpus)
{
	uint32_t i;

	for (i = 0; i < MAX_CPU_CORES; i++)
		cpus->cpus[i] = NULL;
	cpus->idx = 0;
}

static void cpus_do_push(struct cpus *cpus, struct cpu *cpu)
{
	cpus->cpus[cpus->idx++] = cpu;
}

static void cpus_do_free(struct cpus *cpus)
{
        uint32_t i;

	for (i = 0; i < cpus->idx; i++)
		free(cpus->cpus[i]);
	/* free(cpus->cpus); */
}

#define CPTR_TO_NUM(k, value, fn)					\
	do {								\
        char *eptr;							\
	value = fn(k, &eptr, 10);					\
	if (errno == EINVAL || errno == ERANGE)				\
		err(1, "strtoll");					\
	if (k == eptr)							\
		errx(1, "invalid CPU stat value while parsing");	\
	} while (0)

static void parse_cpu_stats(struct cpu *cpu, const char *p)
{
#define NEXT_SPACE(p)					\
	do { for (; !isdigit(*p); p++) ; } while (0)
#define NEXT_DIGIT(p)					\
	do { for (; !isspace(*p); p++) ; } while (0)
#define TAKE_NEXT_ELEM_U64(p, k)					\
	do { NEXT_SPACE(p); NEXT_DIGIT(p); CPTR_TO_NUM(p, k, strtoull); } while (0)
#define TAKE_NEXT_ELEM_I64(p, k)		\
	do { NEXT_SPACE(p); NEXT_DIGIT(p); CPTR_TO_NUM(p, k, strtoll); } while (0);

	NEXT_SPACE(p);
	CPTR_TO_NUM(p, cpu->user, strtoull);
	TAKE_NEXT_ELEM_U64(p, cpu->nice);
	TAKE_NEXT_ELEM_U64(p, cpu->system);
	TAKE_NEXT_ELEM_U64(p, cpu->idle);	
	TAKE_NEXT_ELEM_I64(p, cpu->iowait);
	TAKE_NEXT_ELEM_U64(p, cpu->irq);
	TAKE_NEXT_ELEM_U64(p, cpu->softirq);
	TAKE_NEXT_ELEM_U64(p, cpu->steal);
}

static void collect_cpu_stats(struct cpus *cpus, char *p)
{
	char *sep, *pos0;

	struct cpu *cpu;
	while ((sep = strsep(&p, "\n")) != NULL) {
		if (strlen(sep) > 4 && sep[0] == 'c' &&
		    sep[1] == 'p' && sep[2] == 'u' && !isspace(sep[3])) {
			if ((pos0 = strchr(sep, ' ')) == NULL)
				err(1, "invalid CPU stat value while parsing");
			if ((cpu = malloc(sizeof(struct cpu))) == NULL)
				err(1, "malloc");

			parse_cpu_stats(cpu, pos0);
			cpus_do_push(cpus, cpu);
		}
	}
}

static void get_cpu_stats(struct cpus *cpus)
{
	char *buf;

        memset(cpus, '\0', sizeof(struct cpus));
	cpus_do_init(cpus);
	buf = read_from_file(PROC_STAT);
	collect_cpu_stats(cpus, buf);
        free(buf);
}

static double calc_cpu_usage(struct cpu *prev_cpu, struct cpu *new_cpu)
{
        uint64_t prev_idle, idle;
        uint64_t prev_non_idle, non_idle;
        uint64_t prev_total, total;
	uint64_t total_diff, idle_diff;
	double usage;

	prev_idle = prev_cpu->idle + (uint64_t)prev_cpu->iowait;
	idle = new_cpu->idle + (uint64_t)new_cpu->iowait;

	prev_non_idle = prev_cpu->user + prev_cpu->nice + prev_cpu->system +
		prev_cpu->irq + prev_cpu->softirq + prev_cpu->steal;
	non_idle = new_cpu->user + new_cpu->nice + new_cpu->system +
		new_cpu->irq + new_cpu->softirq + new_cpu->steal;

	prev_total = prev_idle + prev_non_idle;
        total = idle + non_idle;

	total_diff = total - prev_total;
	idle_diff = idle - prev_idle;

	/* To avoid dividing the difference by 0. */ 
	if (total_diff == 0 || idle_diff == 0)
		return (0.0);
	usage = (double)((double)total_diff - idle_diff)/total_diff;
	return (usage*100.0);
}

static inline uint32_t find_cpos_from_p(const char *p, int c)
{
	const char *p0;

	for (p0 = p; *p0 != '\0' && *p0 != c; p0++)
		;
	return ((uint32_t)(p0 - p));
}

static uint64_t get_value_from_meminfo(const char *p, const char *key)
{
	char *v, *k, *dup, *eptr;
	uint64_t value;

        if ((dup = strdup(p)) == NULL)
		err(1, "strdup");
	if ((v = strstr(dup, key)) == NULL)
		errx(1, "'%s' does not exists in /proc/meminfo", key);
	v[find_cpos_from_p(v, (int)'\n')] = '\0';
	for (; !isdigit(*v); v++)
		;
	/* Find a space before the suffix kB. */
	if ((k = memchr(v, ' ', (size_t)UINT32_MAX)) == NULL) {
		/* If the space doesn't exists (value doesn't have a kB suffix)
		   look for a newline. */
		if ((k = memchr(v, '\n', (size_t)(UINT32_MAX))) == NULL)
			errx(1, "can't parse values from %s", PROC_MEMINFO);
	}
	v[k - v] = '\0';
        value = strtoull(v, &eptr, 10);

	if (errno == ERANGE || errno == EINVAL)
		err(1, "strtoull");
	if (eptr == v)
		errx(1, "invalid character while parsing %s",
		     PROC_MEMINFO);
	free(dup);
	return (value);
}

static void parse_meminfo_entries(struct meminfo *memi,
				  const char *mem)
{
	memi->mem_total = get_value_from_meminfo(mem, "MemTotal:");
        memi->mem_used = memi->mem_total - get_value_from_meminfo(
		mem, "MemAvailable:");
        memi->mem_free = get_value_from_meminfo(mem, "MemFree:");
	memi->swap_total = get_value_from_meminfo(mem, "SwapTotal:");
	memi->swap_free = get_value_from_meminfo(mem, "SwapFree:");
	memi->swap_used = memi->swap_total - memi->swap_free;
	/* memi->swap_used = get_value_from_meminfo(mem, ""); */
	/* fprintf(stdout, "mem_total %lu\n", mem_total); */
}

static char *create_bar_line(double usage, const char c)
{
	uint32_t padlen;
	static char padding[BAR_MAX_LEN + 1];

	padlen = (uint32_t)(double)(BAR_MAX_LEN * (usage / 100.0));
	memset(padding, '\0', sizeof(padding));

	memset(padding, c, padlen);
	return (padding);
}

static void do_nanosleep(uint32_t delay)
{
	struct timespec ts;

	ts.tv_nsec = 0;
	ts.tv_sec = delay;
	if (nanosleep(&ts, NULL) < 0)
		err(1, "nanosleep");
}

static uint32_t cptr_to_u32(const char *s)
{
	char *eptr;
	uint64_t val;

	if (*s == '-')
	        errx(1, "values must be greater than or equal to 0");
	val = strtoull(s, &eptr, 10);
	if (errno == EINVAL || errno == ERANGE)
		err(1, "strtoul");
	if (eptr == s)
		errx(1, "no digits were provided");

	if (val >= UINT32_MAX)
		errx(1, "value must be smaller than %u",
		     UINT32_MAX);
	return ((uint32_t)val);
}

/* static char *pretty_bytes(double num) {	 */
/* 	double n, unit, expo; */
/* 	char *buf; */

/* 	n = fabs(num); */
/* 	if (n < 1.0) { */
/* 		return (strdup("0.0B")); */
/* 		/\* snprintf(buf, sizeof(buf), "%lfB", n); *\/ */
/* 		/\* return (buf); *\/ */
/* 	} */

/* 	const char *UNITS[] = {"B", "kB", "MB", "GB", "TB", "PB"}; */
/*         unit = DEFAULT_PRETTY_UNIT; */
/*         expo = MIN((int64_t)floor(log(n) / log(unit)), (int64_t)(ARRAY_SIZE(UNITS) - 1)); */
/* 	fprintf(stdout, "expo: %lf\n", floor(log(n) / log(unit))); */

/*         double pretty = num / pow(unit, expo); */
/*         char *unit_p = UNITS[(uint32_t)expo]; */
/* 	if ((buf = malloc(40)) == NULL) */
/* 		err(1, "malloc"); */
/* 	snprintf(buf, 40, "%.2lf %s", pretty, unit_p); */
/* 	return (buf); */
/* } */

/* Taken from: https://github.com/banyan/rust-pretty-bytes/blob/master/src/converter.rs */
static void pretty_bytes(char *buf, double num) {
	double n, pretty;
	double expos[6];
	/* char *buf; */
	int64_t expo;
        const char *UNITS[] = {
		"B", "kB", "MB", "GB", "TB", "PB"
	};

	n = fabs(num);
	if (n < 1.0) {
		memcpy(buf, "0.0B", 5);
		return;
	}
	if (n < DEFAULT_PRETTY_UNIT) {
		snprintf(buf, BUFLEN, "%lfB", n);
		return;
	}

        expo = MIN((int64_t)floor(log(n) / log(DEFAULT_PRETTY_UNIT)),
		   (int64_t)(ARRAY_SIZE(UNITS) - 1));
        expos[0] = DEFAULT_PRETTY_UNIT;
        expos[1] = expos[0]*DEFAULT_PRETTY_UNIT;
        expos[2] = expos[1]*DEFAULT_PRETTY_UNIT;
        expos[3] = expos[2]*DEFAULT_PRETTY_UNIT;
        expos[4] = expos[3]*DEFAULT_PRETTY_UNIT;
        expos[5] = expos[4]*DEFAULT_PRETTY_UNIT;

        pretty = num / expos[(uint32_t)(expo)-1];
	snprintf(buf, BUFLEN, "%.2lf%s", pretty, UNITS[expo]);
}

static int get_term_nrows(void)
{
	struct winsize w;

	if (ioctl(0, TIOCGWINSZ, &w) < 0)
		err(1, "ioctl");
	return (w.ws_row);
}

/* TODO: prev_cpus, new_cpus as args */
static void print_cpus_usage(struct cpus *prev_cpus, struct cpus *new_cpus,
			     const char pattern, int from, int to,
			     int fd, uint32_t do_clear_screen)
{
	char *pad, *c;
	double usage;
	uint32_t diff, i;
	int rows;

	/* Ignore from index if to contains a negative value. */
	if (to < 0) {
		to = prev_cpus->idx;
		from = 0;
	}
	if ((uint32_t)to > prev_cpus->idx)
		errx(1, "you only have %u cores (including HT)",
		     prev_cpus->idx);

	if (do_clear_screen)
		fprintf(stdout, CLEAR_RESET_CURSOR);

	fflush(stdout);

	/* To avoid going outside of the terminal's visible screen
	   when system has many active cores. */
	rows = get_term_nrows();
	if (to >= (rows>>1))
		to = rows>>1;
	for (i = from; (int)i < to; i++) {
		usage = calc_cpu_usage(prev_cpus->cpus[i],
				       new_cpus->cpus[i]);
		pad = create_bar_line(usage, pattern);
		diff = BAR_MAX_LEN-(uint32_t)strlen(pad);

		if (usage >= 40.0)      c = COLOR_LIGHT_RED;
		else if (usage >= 80.0) c = COLOR_INTENSE_RED;
		else                    c = COLOR_GREEN;

		if (fd == -1) {			
			fprintf(stdout, "%s%c%s [%s%s%s%*s]",
				COLOR_LIGHT_PINK, pattern, COLOR_END,
				COLOR_GREEN, pad, COLOR_END, diff, "");
			fprintf(stdout, " %s%.2lf%%%s (%u)\r\n", c, usage,
				COLOR_END, i);
		} else {
			dprintf(fd, "%c [%s%*s]", pattern, pad, diff, "");
			dprintf(fd, " %.2lf%% (%u)\r\n", usage, i);
		}
	}
}

static void print_memory_info(struct meminfo *memi, const char pattern,
			      int fd, uint32_t do_clear_screen)
{
	char *pad, *c;
	double usage;
	uint32_t diff;
	char total[40], used[40];

	if (do_clear_screen && fd == -1)
		fprintf(stdout, CLEAR_RESET_CURSOR);

	usage = ((double)memi->mem_used/(double)memi->mem_total)*100.0;
	if (usage >= 50.0)      c = COLOR_LIGHT_RED;
	else if (usage >= 90.0) c = COLOR_INTENSE_RED;
	else                    c = COLOR_GREEN;

	pad = create_bar_line(usage, pattern);
	diff = BAR_MAX_LEN-(uint32_t)strlen(pad);
	memset(total, '\0', sizeof(total));
	memset(used, '\0', sizeof(used));
        pretty_bytes(total, memi->mem_total*DEFAULT_PRETTY_UNIT);
	pretty_bytes(used, memi->mem_used*DEFAULT_PRETTY_UNIT);
	if (fd > -1)
		dprintf(fd, "%c Mem:  [%s%*s] (%s/%s) (%.2lf%%)\n",
			pattern, pad, diff, "", used, total, usage);
	else
		fprintf(stdout, "%s%c%s Mem:  [%s%s%s%*s] (%s%s%s/%s%s%s) (%s%.2lf%%%s)\n",
			COLOR_GREEN, pattern, COLOR_END, COLOR_GREEN,
			pad, COLOR_END, diff, "", COLOR_WHITE, used, COLOR_END,
			COLOR_WHITE, total, COLOR_END, c, usage, COLOR_END);

	usage = (memi->swap_used/(double)memi->swap_total)*100.0;
	if (usage >= 50.0)      c = COLOR_LIGHT_RED;
	else if (usage >= 90.0) c = COLOR_INTENSE_RED;
	else                    c = COLOR_GREEN;

	pad = create_bar_line(usage, pattern);
	diff = BAR_MAX_LEN-(uint32_t)strlen(pad);
	memset(total, '\0', sizeof(total));
	memset(used, '\0', sizeof(used));
	pretty_bytes(total, memi->swap_total*DEFAULT_PRETTY_UNIT);
	pretty_bytes(used, memi->swap_used*DEFAULT_PRETTY_UNIT);
	if (fd > -1)
		dprintf(fd, "%c Swap: [%s%*s] (%s/%s) (%.2lf%%)\n",
			pattern, pad, diff, "", used, total, usage);
	else
		fprintf(stdout, "%s%c%s Swap: [%s%s%s%*s] (%s%s%s/%s%s%s) (%s%.2lf%%%s)\n",
			COLOR_GREEN, pattern, COLOR_END, COLOR_GREEN,
			pad, COLOR_END, diff, "", COLOR_WHITE, used,
			COLOR_END, COLOR_WHITE, total, COLOR_END, c,
		usage, COLOR_END);
}

static int socket_do_init(void)
{
	struct sockaddr_in addr_in;
	/* int fd, afd; */
	int fd, e;
	
	addr_in.sin_family = AF_INET;
	addr_in.sin_port = htons(5000);
	addr_in.sin_addr.s_addr = INADDR_ANY;
	memset(&addr_in.sin_zero, '\0', sizeof(addr_in.sin_zero));

	e = 1;
	if ((fd = socket(AF_INET, SOCK_STREAM, 6)) == -1)
		err(1, "socket");
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &e, sizeof(int)) < 0)
		err(1, "setsockopt");
	if (bind(fd, (const struct sockaddr *)&addr_in,
		 sizeof(struct sockaddr)) < 0)
		err(1, "bind");
	if (listen(fd, MAX_LISTEN_QUEUE) < 0)
		err(1, "listen");

	return (fd);
}

static void parse_command_args(const char *arg0, const char *arg1,
			       uint32_t only_cpu, int *from, int *to,
			       uint32_t *delay, int *socket_fd, char *pat)
{
	char *sp;

	if (only_cpu && strncmp(arg0, "-cores", 6) == 0) {
		if (arg1 == NULL)
			errx(1, "number of cores are not specified");
		/* Check if the argument is 'n-n+1', and if it's not,
		   then it's we're specific about a core. */
	        if ((sp = memchr(arg1, '-',
				 strlen(arg1))) == NULL) {
			CPTR_TO_NUM(arg1, *to, strtol);
			if (*to < 0) *from = *to = -1;
			else {
				*from = *to;
				*to += 1;
			}
		} else {
			CPTR_TO_NUM(arg1, *from, strtol);
			CPTR_TO_NUM(arg1 + (sp - arg1) + 1, *to, strtol);
			if (*from == *to)
				errx(1, "range from..to cannot be same");
		}
	}

	if (strncmp(arg0, "-delay", 6) == 0) {
		if (arg1 == NULL)
			errx(1, "delay is not provided");
		*delay = cptr_to_u32(arg1);
	}

	if (strncmp(arg0, "-pattern", 8) == 0) {
		if (arg1 == NULL)
			errx(1, "pattern is not provided");
		/* Pattern must be a single character only.
		   Everything after the first character is rejected. */
		*pat = *arg1;
	}

	if (strncmp(arg0, "-server", 7) == 0) {
		fprintf(stdout, "%s*%s listening on localhost:5000\n",
			COLOR_GREEN, COLOR_END);
		*socket_fd = socket_do_init();
	}
}

static int socket_accept_from(int socket_fd)
{
	struct sockaddr_in addr_acpt;
	int fd;
	socklen_t len;

	memset(&addr_acpt, '\0', sizeof(struct sockaddr_in));
	len = sizeof(struct sockaddr);
        if ((fd = accept(
		     socket_fd, (struct sockaddr *)&addr_acpt,
		     &len)) < 0)
		err(1, "accept");
	return (fd);
}

static void ignore_sigpipe(int signo, siginfo_t *info, void *ctx)
{
	(void)signo;
	(void)info;
	(void)ctx;
}

static void ignore_sigpipe_signal(void)
{
	struct sigaction act;

	memset(&act, '\0', sizeof(struct sigaction));
	act.sa_sigaction = &ignore_sigpipe;
	if (sigaction(SIGPIPE, &act, NULL) < 0)
		err(1, "sigaction");
}

static void print_help(void)
{
	fprintf(stdout,
		"cstat - display CPU/memory usage\n"
		" cpu  - display CPU usage\n"
		" mem  - display memory usage\n"
		"options\n"
		" -server  - send usage information to localhost:5000\n"
		" -cores   - specify number of CPU cores\n"
		" -delay   - specify delay between refresh\n"
		" -pattern - specify a pattern\n");
}

int main(int argc, char **argv)
{
        struct cpus prev_cpus, new_cpus;
	char *mem;
	struct meminfo memi;
        char pattern;
        int from, to, i;

	uint32_t delay;

	int socket_fd, conn_fd;

	pattern = '*';
	delay = 1;
	from = to = -1;
	socket_fd = conn_fd = -1;

	ignore_sigpipe_signal();
	if (argc > 1 && strncmp(argv[1], "cpu", 3) == 0) {
		for (i = 1; i < argc; i++)
			parse_command_args(argv[i], argv[i+1], 1,
					   &from, &to, &delay,
					   &socket_fd, &pattern);
		for (;;) {
			/* We initialized the socket in parse_command_args(). */
			if (socket_fd > -1)
				conn_fd = socket_accept_from(socket_fd);
			get_cpu_stats(&prev_cpus);
			do_nanosleep(delay);
			get_cpu_stats(&new_cpus);
			print_cpus_usage(&prev_cpus, &new_cpus,
					 pattern, from, to, conn_fd,
					 conn_fd > -1 ? 0 : 1);
			if (conn_fd > -1) {
				shutdown(conn_fd, SHUT_WR);
				close(conn_fd);
				conn_fd = -1;
			}
			cpus_do_free(&prev_cpus);
			cpus_do_free(&new_cpus);
		}
		/* } */
	} else if (argc > 1 && strncmp(argv[1], "mem", 3) == 0) {
		for (i = 1; i < argc; i++)
			parse_command_args(argv[i], argv[i+1], 0,
					   &from, &to, &delay, &socket_fd,
					   &pattern);
		for (;;) {
			if (socket_fd > -1)
				conn_fd = socket_accept_from(socket_fd);
			mem = read_from_file(PROC_MEMINFO);
			parse_meminfo_entries(&memi, mem);
			print_memory_info(&memi, pattern, conn_fd,
					  conn_fd > -1 ? 0 : 1);
			do_nanosleep(delay);
			if (conn_fd > -1) {
				shutdown(conn_fd, SHUT_WR);
				close(conn_fd);
				conn_fd = -1;
			}
			free(mem);
		}
	} else if (argc > 1 && strncmp(argv[1], "help", 4) == 0) {
		print_help();
	} else {
		for (i = 1; i < argc; i++)
			parse_command_args(argv[i], argv[i+1], 1,
					   &from, &to, &delay, &socket_fd,
					   &pattern);
		for (;;) {
			if (socket_fd > -1)
				conn_fd = socket_accept_from(socket_fd);
		        get_cpu_stats(&prev_cpus);
			do_nanosleep(delay);
			get_cpu_stats(&new_cpus);
			mem = read_from_file(PROC_MEMINFO);
			parse_meminfo_entries(&memi, mem);
			print_cpus_usage(&prev_cpus, &new_cpus,
					 pattern, from, to, conn_fd,
					 conn_fd > -1 ? 0 : 1);
			if (conn_fd > -1)
				write(conn_fd, "\n", 1);
			else
				fputc('\n', stdout);
			print_memory_info(&memi, pattern, conn_fd,
					  conn_fd > -1 ? 0 : 1);
			if (conn_fd > -1) {
				shutdown(conn_fd, SHUT_WR);
				close(conn_fd);
				conn_fd = -1;
			}
			cpus_do_free(&prev_cpus);
			cpus_do_free(&new_cpus);
			free(mem);
		}
	}
}
