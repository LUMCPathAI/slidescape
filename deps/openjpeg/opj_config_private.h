#ifndef OPJ_CONFIG_PRIVATE_H_INCLUDED
#define OPJ_CONFIG_PRIVATE_H_INCLUDED

#define OPJ_PACKAGE_VERSION "2.5.4"

#if (defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
		__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) || defined(__BIG_ENDIAN__)
#define OPJ_BIG_ENDIAN
#endif

#endif
