// czsc-cpp 顶层聚合头文件
// 一行 #include <czsc/czsc.h> 即可获取全部类型+算法+TA
#pragma once

#include "czsc/types/enums.hpp"
#include "czsc/types/raw_bar.hpp"
#include "czsc/types/new_bar.hpp"
#include "czsc/types/fx.hpp"
#include "czsc/types/bi.hpp"
#include "czsc/types/zs.hpp"
#include "czsc/types/signal.hpp"

#include "czsc/ta/ta_cache.hpp"
#include "czsc/ta/indicators.hpp"

#include "czsc/analyze/algorithms.hpp"
#include "czsc/analyze/czsc.hpp"

#include "czsc/signals/param_view.hpp"
#include "czsc/signals/signal_builder.hpp"
#include "czsc/signals/registry.hpp"

#include "czsc/io/data_loader.hpp"
