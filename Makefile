all: compile

deps: nif

nif:
	$(MAKE) -C c_src nif

compile: nif
	rebar3 compile

clean:
	rebar3 clean
	$(MAKE) -C c_src clean

distclean:
	$(MAKE) -C c_src distclean

test:
	rebar3 eunit

benchmark:
	erl -noshell -pa _build/test/lib/*/ebin -pa _build/test/lib/glazejson/test \
    -eval "glazejson_bench:run(), halt()."

.PHONY: all deps compile clean distclean test nif
