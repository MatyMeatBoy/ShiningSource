#ifndef SHININGSOURCE_INI_H
#define SHININGSOURCE_INI_H

/* Both the port's key=value and RecompLauncher's key = value are valid. */
static inline const char *ss_ini_key_end(const char *key, const char *equals)
{
    while (equals > key && (equals[-1] == ' ' || equals[-1] == '\t'))
        --equals;
    return equals;
}

static inline const char *ss_ini_value(const char *equals)
{
    const char *value = equals + 1;
    while (*value == ' ' || *value == '\t')
        ++value;
    return value;
}

#endif
