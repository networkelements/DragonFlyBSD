PROG=	hammer
SRCS=	hammer.c ondisk.c blockmap.c cache.c misc.c cycle.c uuid.c \
	cmd_show.c cmd_softprune.c cmd_history.c \
	cmd_blockmap.c cmd_reblock.c cmd_rebalance.c \
	cmd_synctid.c cmd_stats.c cmd_remote.c \
	cmd_pfs.c cmd_snapshot.c cmd_mirror.c \
	cmd_cleanup.c cmd_info.c cmd_version.c cmd_volume.c \
	cmd_config.c cmd_recover.c cmd_dedup.c cmd_abort.c \
	cmd_strip.c
MAN=	hammer.8

CFLAGS+= -I${.CURDIR}/../../sys
LDADD=	-lm -lutil -lhammer -lprivate_crypto
DPADD=	${LIBM} ${LIBUTIL} ${LIBHAMMER} ${LIBRECRYPTO}
LDFLAGS+=	${PRIVATELIB_LDFLAGS}

.PATH: ${.CURDIR}/../../sys/libkern
SRCS+= crc32.c icrc32.c

.include <bsd.prog.mk>
