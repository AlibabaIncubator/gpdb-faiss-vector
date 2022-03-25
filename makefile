EXTENSION = vector_recall
DATA = vector_recall--*.sql
MODULE_big = vector_recall
OBJS = vector_recall.o $(CACHE)/libcache.a
REGRESS = vector_recall

CACHE = cache
FAISS = faiss

PG_CPPFLAGS = -I$(FAISS)/c_api
PG_LDFLAGS = -L$(FAISS)/build/c_api
SHLIB_LINK = -lfaiss_c

DEPS = $(FAISS)/build/c_api/libfaiss_c.so

PREPARE: $(DEPS)
	$(INSTALL_SHLIB) -C $^ $(shell ldd $^ | sed 's:^[^/]*::'| sed 's:(.*)::'|sed '/:/d'|grep -v ld-linux) '$(DESTDIR)$(pkglibdir)/../'
.PHONY: PREPARE

$(DEPS):
	cd $(FAISS) && cmake3 -DFAISS_ENABLE_GPU=OFF -DFAISS_ENABLE_PYTHON=OFF -DFAISS_ENABLE_C_API=ON -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release -DFAISS_OPT_LEVEL=avx2 -B build . && make -C build -j4

$(CACHE)/libcache.a:
	cd $(CACHE) && make

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
