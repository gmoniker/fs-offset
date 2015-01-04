#ifndef ACL_FUNCTIONS_H
#define ACL_FUNCTIONS_H

extern _Bool has_extended_acls(int fd);
extern _Bool has_default_acl(const char *dir);
extern _Bool has_extended_acls_lpath(const char *path, _Bool isdir);
extern _Bool rebase_acl(int64_t difference, const char *path, bool isdir);
#endif
