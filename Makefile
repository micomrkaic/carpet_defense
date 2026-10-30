# ====================================================================
#  The Housekeeper  --  Makefile
#
#  make              native build  ->  ./housekeeper
#  make run          build and run
#  make install      install to $(PREFIX)/bin + man page  (default ~/.local)
#  make uninstall
#  make wasm         browser build -> docs/   (GitHub Pages serves docs/)
#  make serve        serve docs/ on http://localhost:8000
#  make man          preview the man page
#  make clean
#
#  Override the prefix:   make install PREFIX=/usr/local
# ====================================================================

CC       ?= gcc
CFLAGS   ?= -std=c17 -O2 -Wall -Wextra -Wno-unused-parameter
SDL_LIBS := $(shell sdl2-config --cflags --libs)
LIBS      = $(SDL_LIBS) -lm

EMCC     ?= emcc
# Fixed 64 MB heap, no growth: ALLOW_MEMORY_GROWTH on Emscripten 6.x uses a
# resizable ArrayBuffer, which TextDecoder rejects -> WebGL init crashes.
EMFLAGS   = -std=c17 -O2 -s USE_SDL=2 -s INITIAL_MEMORY=64MB -s ALLOW_MEMORY_GROWTH=0

PREFIX   ?= $(HOME)/.local
BINDIR    = $(PREFIX)/bin
MANDIR    = $(PREFIX)/share/man/man6

SRC       = housekeeper.c
TARGET    = housekeeper
MANPAGE   = housekeeper.6
SHELL_HTML= shell.html
WASM_DIR  = docs
WASM_OUT  = $(WASM_DIR)/index.html

.PHONY: all run install uninstall wasm serve man clean

# -- native -----------------------------------------------------------
all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

run: $(TARGET)
	./$(TARGET)

# -- install ----------------------------------------------------------
install: $(TARGET) $(MANPAGE)
	install -Dm755 $(TARGET)  $(DESTDIR)$(BINDIR)/$(TARGET)
	install -Dm644 $(MANPAGE) $(DESTDIR)$(MANDIR)/$(MANPAGE)
	@echo "installed $(BINDIR)/$(TARGET) and $(MANDIR)/$(MANPAGE)"
	@case ":$$PATH:" in *":$(BINDIR):"*) ;; \
	  *) echo "note: $(BINDIR) is not in your PATH" ;; esac

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET) $(DESTDIR)$(MANDIR)/$(MANPAGE)

man: $(MANPAGE)
	man ./$(MANPAGE)

# -- WebAssembly (needs emcc; source ~/emsdk/emsdk_env.sh first) -------
wasm: $(WASM_OUT)

$(WASM_OUT): $(SRC) $(SHELL_HTML)
	@mkdir -p $(WASM_DIR)
	$(EMCC) $(EMFLAGS) --shell-file $(SHELL_HTML) -o $@ $<
	@touch $(WASM_DIR)/.nojekyll
	@echo "WASM build OK -> $(WASM_DIR)/   (make serve to test)"

serve: $(WASM_OUT)
	@echo "http://localhost:8000   (Ctrl-C to stop)"
	python3 -m http.server 8000 --directory $(WASM_DIR)

# -- clean ------------------------------------------------------------
clean:
	rm -f $(TARGET) $(WASM_DIR)/index.html $(WASM_DIR)/index.js $(WASM_DIR)/index.wasm
