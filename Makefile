# 外壳源码链接同目录的三个 so。因子、触发、模型的源码不在这里。
# cell 框架仍在仓库的 sp_cpp/cell。

CELL = $(abspath ../../sp_cpp/cell)
CXX = g++
CXXFLAGS = -std=c++17 -g -O2 -Wall -Wextra
INC = -I. -I$(CELL)/base/common -I$(CELL)/base/strategy -I$(CELL)/third/include
LIBS = liblfm_factor.so liblfm_trigger.so liblfm_model.so
SHELL_SRCS = lfm_strategy_main.cpp lfm_strategy.cpp lfm_parq_stra.cpp csv_table.cpp

.PHONY: all clean

all: lfm_stra

lfm_stra: $(SHELL_SRCS) $(LIBS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $(SHELL_SRCS) -L. -llfm_factor -llfm_trigger -llfm_model -Wl,-rpath,'$$ORIGIN' -pthread -ldl -lrt -lparquet -larrow

$(LIBS):
	$(error 缺少 $@，把编好的 so 放在本目录)

clean:
	rm -f lfm_stra
