cookies_CFLAGS := -fvisibility=hidden -fPIC
cookies_SOURCES := modules/cookies/cookies.c
cookies_DEPEND := $(libkres)
cookies_LIBS := $(contrib_TARGET) $(libkres_TARGET) $(libkres_LIBS)
$(call make_c_module,cookies)
