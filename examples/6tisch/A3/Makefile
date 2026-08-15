CONTIKI_PROJECT = node
all: $(CONTIKI_PROJECT)
CONTIKI = ../../..

MAKE_MAC = MAKE_MAC_TSCH
MAKE_ROUTING = MAKE_ROUTING_RPL_CLASSIC
CFLAGS += -DROUTING_CONF_RPL_CLASSIC=1
MODULES += os/net/routing/rpl-classic
# orchestra framework + EB/common rules (existing ALICE module, untouched)
MODULES += os/services/alice
# A3 core + ALICE-A3 adaptive unicast scheduler (new module)
MODULES += os/services/alice-a3

MAKE_WITH_PERIODIC_ROUTES_PRINT ?= 1
ifeq ($(MAKE_WITH_PERIODIC_ROUTES_PRINT),1)
CFLAGS += -DWITH_PERIODIC_ROUTES_PRINT=1
endif
include $(CONTIKI)/Makefile.include
