#define _XOPEN_SOURCE 500
#include <features.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdio.h>
#include <sys/acl.h> 
#include <stdbool.h>
#include <sys/types.h>
#include <errno.h>
#include <attr/xattr.h>

int rebase_qualified_entries(acl_t acl, int64_t difference) {
	acl_entry_t acl_entry;
	acl_tag_t acl_tag;
	int ent, result;
	uid_t uid_t, *uid;
	gid_t gid_t, *gid;
	int64_t id;
	bool changed = false;
	for (ent = ACL_FIRST_ENTRY; ; ent = ACL_NEXT_ENTRY) {
		if (-1 == (result = acl_get_entry(acl, ent, &acl_entry))) {
			perror("acl_get_entry");
			exit(EXIT_FAILURE);
		}
		if (!result) break;
		if (-1 == acl_get_tag_type(acl_entry, &acl_tag)) {
			perror("acl_get_tag_type");
			exit(EXIT_FAILURE);
		}
		if (acl_tag == ACL_USER) {
			if (NULL == (uid = acl_get_qualifier(acl_entry))) {
				perror("acl_get_qualifier");
				exit(EXIT_FAILURE);
			}
			id = *uid + difference;
			if (-1 == acl_free(uid)) {
				perror("acl_free");
				exit(EXIT_FAILURE);
			}
			if (id < 0 || id > UINT32_MAX) {
				errno = ERANGE;
				return false;	
			}
			uid_t = id;
			if (-1 == acl_set_qualifier(acl_entry, &uid_t)) {
				perror("acl_set_qualifier");
				exit(EXIT_FAILURE);
			}
			changed = true;
		} else if (acl_tag == ACL_GROUP) {
			if (NULL == (gid = acl_get_qualifier(acl_entry))) {
				perror("acl_get_qualifier");
				exit(EXIT_FAILURE);
			}
			id = *gid + difference;
			if (-1 == acl_free(gid)) {
				perror("acl_free");
				exit(EXIT_FAILURE);
			}
			if (id < 0 || id > UINT32_MAX) {
				errno = ERANGE;
				return false;	
			}
			gid_t = id;
			if (-1 == acl_set_qualifier(acl_entry, &gid_t)) {
				perror("acl_set_qualifier");
				exit(EXIT_FAILURE);
			}
			changed = true;
		} 	
	}
	if (changed) {
		return 2;
	} else {
		return 1;
	}
}

bool rebase_acl(int64_t difference, const char *path, bool isdir) {
	acl_t acl;
	int result;
	if (isdir) {
		if (NULL == (acl = acl_get_file(path, ACL_TYPE_DEFAULT))) {
			perror("acl_get_file");
			exit (EXIT_FAILURE);
		}
		if (false == (result = rebase_qualified_entries(acl, difference))) {
			return false;
		} else {
			if (2 == result) {
				if (-1 == acl_set_file(path, ACL_TYPE_DEFAULT, acl)) {
					perror("acl_set_file");
					exit(EXIT_FAILURE);
				}
			}
			if (-1 == acl_free(acl)) {
				perror("acl_free");
				exit(EXIT_FAILURE);
			}
		}
	}
	if (NULL == (acl = acl_get_file(path, ACL_TYPE_ACCESS))) {
		perror ("acl_get_file");
		exit (EXIT_FAILURE);
	}
	if (false == (result = rebase_qualified_entries(acl, difference))) {
		return false;
	} else {
		if (2 == result) {
			if (-1 == acl_set_file(path, ACL_TYPE_ACCESS, acl)) {
				perror("acl_set_file");
				exit(EXIT_FAILURE);
			}
		}
		if (-1 == acl_free(acl)) {
			perror("acl_free");
			exit(EXIT_FAILURE);
		}
	}
	return true;
}

bool
has_default_acl(const char *dir) {
	acl_t acl;
	if (NULL == (acl = acl_get_file(dir, ACL_TYPE_DEFAULT))) {
		//It seems that the absence of a default access acl can only be detected as EACCES
		if (errno == ENOTSUP || errno == EACCES) {
			return false;
		} else {
			perror ("acl_get_file");
			exit (EXIT_FAILURE);
		}
	}
	if (acl) {
		if (-1 == acl_free(acl)) {
			perror("acl_free");
			exit(EXIT_FAILURE);
		}
		return true;
	}
	return false;
}

bool
has_named_attribute_lpath(const char *path, const char *name) {
	ssize_t esize;
	char buf[1];
	esize = lgetxattr(path, name, &buf, sizeof buf);
	if (esize == -1) {
		if (errno == ENOATTR || errno == ENOTSUP) {
			return false;
		} else if (errno == ERANGE){
			return true;
		} else {
			perror("lgetxattr");
			exit(EXIT_FAILURE);
		}
	} else {
		return true;
	}
}

bool
has_extended_acls_lpath(const char *path, bool isdir) {
	//Don't use use this function on a symlink. These values wouldn't be active ACLs
	//At least not on Linux. On MAC OSX maybe.
	//Or on a filesystem where acls are not implemented in extended attributes
	if (has_named_attribute_lpath(path, "system.posix_acl_access")) {
		return true;
	} else if(isdir) {
		if (has_named_attribute_lpath(path, "system.posix_acl_default")) {
			return true;
		} else {
			return false;
		}
	} else {
		return false;
	}
}

bool
has_extended_acls(int fd) {
	acl_t acl;
	acl_entry_t acl_entry;
	acl_tag_t acl_tag;
	int ent, result;
	if (NULL == (acl = acl_get_fd(fd))) {
		if (errno == ENOTSUP) {
			return false;
		} else {
			perror ("acl_get_file");
			exit (EXIT_FAILURE);
		}
	}
	for (ent = ACL_FIRST_ENTRY; ; ent = ACL_NEXT_ENTRY) {
		if (-1 == (result = acl_get_entry(acl, ent, &acl_entry))) {
			perror("acl_get_entry");
			exit(EXIT_FAILURE);
		}
		if (!result) break;
		if (-1 == acl_get_tag_type(acl_entry, &acl_tag)) {
			perror("acl_get_tag_type");
			exit(EXIT_FAILURE);
		}
		if (acl_tag == ACL_USER || acl_tag == ACL_GROUP) {
			if (-1 == acl_free(acl)) {
				perror("acl_free");
				exit(EXIT_FAILURE);
			}
			return true;
		}

	}
	if (acl) {
		if (-1 == acl_free(acl)) {
			perror("acl_free");
			exit(EXIT_FAILURE);
		}
	}
	return false;
}
