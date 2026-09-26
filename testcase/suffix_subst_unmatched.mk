value := item.h

all:
	@echo matched=$(value:.h=.h.d) unmatched=$(value:.h.d=.h) appended=$(value:=.d)
