OBJECTS_SRC = ${OBJECTS:C/\.o/.cpp/g}

GITINFO != GIT_CEILING_DIRECTORIES=`pwd`/.. git describe --always 2>/dev/null || true

.if exists(.localflags)
.  include ".localflags"
.endif

.if exists(.cflags)
.  include ".cflags"
.endif

.if $(GITINFO) != ""
    CPPFLAGS += -DGITINFO="\"$(GITINFO)\""
.endif

ALPM ?= no
.if $(ALPM) == yes
CPPFLAGS += -DWITH_ALPM
LIBS += -lalpm
.endif

REGEX ?= no
.if $(REGEX) == yes
CPPFLAGS += -DWITH_REGEX
.endif

THREADS ?= no
.if $(THREADS) == yes
CPPFLAGS += -DENABLE_THREADS
.endif

.include "Makefile"

.if !defined(ALLFLAGS) || !defined(OLDCXX) \
    || "$(ALLFLAGS)" != "$(COMPAREFLAGS)" \
    || "$(OLDCXX)" != "$(CXX)"
.PHONY: .cflags
# the first echo needs to use single quotes otherwise the .if
# comparisons fail
.cflags:
	@echo 'ALLFLAGS := $(COMPAREFLAGS)' > .cflags
	@echo "OLDCXX := $(CXX)" >> .cflags
.endif
