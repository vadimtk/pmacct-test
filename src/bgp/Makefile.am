AM_CFLAGS = $(PMACCT_CFLAGS)

noinst_LTLIBRARIES = libbgp.la
libbgp_la_SOURCES = bgp.c bgp_aspath.c bgp_community.c			\
	bgp_ecommunity.c bgp_hash.c bgp_prefix.c bgp_table.c		\
	bgp_logdump.c bgp_util.c bgp_msg.c bgp_lookup.c			\
	bgp_lcommunity.c bgp_recvs.c bgp_aspath.h bgp_community.h	\
	bgp_ecommunity.h bgp.h bgp_hash.h bgp_logdump.h			\
	bgp_lookup.h bgp_msg.h bgp_packet.h bgp_prefix.h		\
	bgp_table.h bgp_util.h bgp_lcommunity.h bgp_recvs.h		\
	bgp_recvs-data.h

libbgp_la_CFLAGS = -I$(srcdir)/.. $(AM_CFLAGS)
