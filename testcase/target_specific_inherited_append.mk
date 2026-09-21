GLOBAL_CPPFLAGS := -Iglobal/include -DGLOBAL

all: target

target: override GLOBAL_CPPFLAGS := -Itarget/include $(GLOBAL_CPPFLAGS)
target:
	@echo $(GLOBAL_CPPFLAGS)
