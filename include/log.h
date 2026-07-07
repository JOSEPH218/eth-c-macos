#ifndef ETHC_LOG_H
#define ETHC_LOG_H

void log_info(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_error(const char *fmt, ...);
void log_set_verbose(int verbose);

#endif /* ETHC_LOG_H */
