LDFLAGS=-lm -std=c89 -pedantic -D_GNU_SOURCE

compile:config.h config.c common.c common.h ledger.c ledger.h user.c node.c main.c
	gcc config.h config.c common.c common.h ledger.c ledger.h user.c -o user $(LDFLAGS)
	gcc config.h config.c common.c common.h ledger.c ledger.h node.c -o node $(LDFLAGS)
	gcc config.h config.c common.c common.h ledger.c ledger.h main.c -o master $(LDFLAGS)

exec:config.h config.c common.c common.h ledger.c ledger.h user.c node.c main.c
	gcc config.h config.c common.c common.h ledger.c ledger.h user.c -o user $(LDFLAGS)
	gcc config.h config.c common.c common.h ledger.c ledger.h node.c -o node $(LDFLAGS)
	gcc config.h config.c common.c common.h ledger.c ledger.h main.c -o master $(LDFLAGS)
	./master

clear:
	rm master user node