/* auth.h - /config/auth 认证（SHA3-256 + 盐） */
#ifndef AUTH_H
#define AUTH_H

#include "types.h"

/* 成功返回 0；-1 没挂载/-2 没文件/-3 没找到用户/-4 密码错 */
int auth_check(const char *user, const char *pass);
const char *auth_source(void);
const char *auth_name(void);
int auth_role(void);   /* 1 = administrator */

#endif
