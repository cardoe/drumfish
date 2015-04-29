.PHONY: all
all:
	@test -e simavr/Makefile || ( \
		git submodule init && \
		git submodule update )
	$(MAKE) -C simavr build-simavr
	$(MAKE) -C src

.PHONY: clean
clean:
	$(MAKE) -C simavr clean
	$(MAKE) -C src clean

.PHONY: test
test:
	$(MAKE) -C tests test
