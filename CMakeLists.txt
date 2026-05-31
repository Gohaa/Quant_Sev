cmake_minimum_required(VERSION 3.16)
project(Quant_Sev VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

set(QUANT_SEV_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

add_library(quant_sev_third_party INTERFACE)
target_include_directories(quant_sev_third_party INTERFACE
    ${QUANT_SEV_THIRD_PARTY}
    ${QUANT_SEV_THIRD_PARTY}/nlohmann
    ${QUANT_SEV_THIRD_PARTY}/httplib
)

add_library(quant_sev_common STATIC Core/Logger/Logger.cpp)
target_include_directories(quant_sev_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Core)
target_link_libraries(quant_sev_common PUBLIC quant_sev_third_party)

add_library(quant_sev_bll STATIC
    BLL/Common/ContractRules.cpp
    BLL/Common/OptionRules.cpp
    BLL/Storage/StorageEngine.cpp
    BLL/Bar/BarPeriod.cpp
    BLL/Bar/BarEngine.cpp
    BLL/Indicator/IndicatorEngine.cpp
    BLL/Indicator/IndicatorCompute.cpp
    BLL/Strategy/StrategyRuntime.cpp
    BLL/Strategy/C_Strategy/DualThrust.cpp
    BLL/Strategy/C_Strategy/MaCross.cpp
    BLL/Strategy/C_Strategy/Macd.cpp
    BLL/Strategy/C_Strategy/DoubleEma.cpp
    BLL/Strategy/C_Strategy/Boll.cpp
    BLL/Strategy/StrategyEngine.cpp
    BLL/Backtest/MatchEngine.cpp
    BLL/Backtest/BacktestEngine.cpp
    BLL/Backtest/TickBarSynthesizer.cpp
    BLL/OrderBook/OrderBookEngine.cpp
    BLL/Rollover/RolloverEngine.cpp
    BLL/Indicator/tulipindicators/tiamalgamation.c
)
target_include_directories(quant_sev_bll
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/BLL
        ${CMAKE_CURRENT_SOURCE_DIR}/Core
        ${CMAKE_CURRENT_SOURCE_DIR}/BLL/Indicator/tulipindicators
)
if(MSVC)
    set_source_files_properties(BLL/Indicator/tulipindicators/tiamalgamation.c PROPERTIES COMPILE_FLAGS "/w")
endif()
target_link_libraries(quant_sev_bll PUBLIC quant_sev_third_party quant_sev_common)

add_library(quant_sev_core STATIC
    Core/Account/AccountRecord.cpp
    Core/Account/AccountService.cpp
    Core/Config/Config.cpp
    Core/Gateway/Gateway.cpp
    Core/Quote/QuoteEngine.cpp
    Core/Symbol/SymbolService.cpp
    Core/Trade/TradeEngine.cpp
    Core/CTA/CtaEngine.cpp
    Core/Risk/RiskEngine.cpp
    Core/TimeCheck/TimeCheckEngine.cpp
)
target_include_directories(quant_sev_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/Core
        ${CMAKE_CURRENT_SOURCE_DIR}/BLL
)
target_link_libraries(quant_sev_core PUBLIC quant_sev_third_party quant_sev_common quant_sev_bll)
target_compile_definitions(quant_sev_core PUBLIC QUANT_SEV_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")

set(QUANT_SEV_CTP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/CTP" CACHE PATH "CTP SDK root (headers)")
set(QUANT_SEV_CTP_LIB_DIR "${QUANT_SEV_CTP_ROOT}/lib" CACHE PATH "CTP import libraries directory")

find_library(QUANT_SEV_MD_LIB NAMES thostmduserapi_se thostmduserapi PATHS ${QUANT_SEV_CTP_LIB_DIR})
find_library(QUANT_SEV_TD_LIB NAMES thosttraderapi_se thosttraderapi PATHS ${QUANT_SEV_CTP_LIB_DIR})

set(_ctp_default OFF)
if(QUANT_SEV_MD_LIB AND QUANT_SEV_TD_LIB)
    set(_ctp_default ON)
endif()
option(QUANT_SEV_ENABLE_CTP "Link CTP Md/Trader API libraries" ${_ctp_default})

if(QUANT_SEV_ENABLE_CTP)
    target_include_directories(quant_sev_core PUBLIC ${QUANT_SEV_CTP_ROOT})
    if(QUANT_SEV_MD_LIB AND QUANT_SEV_TD_LIB)
        target_compile_definitions(quant_sev_core PUBLIC QUANT_SEV_HAS_CTP)
        target_link_libraries(quant_sev_core PUBLIC ${QUANT_SEV_MD_LIB} ${QUANT_SEV_TD_LIB})
        message(STATUS "CTP enabled: ${QUANT_SEV_MD_LIB}; ${QUANT_SEV_TD_LIB}")
    else()
        message(WARNING "QUANT_SEV_ENABLE_CTP=ON but libraries not found in ${QUANT_SEV_CTP_LIB_DIR}")
    endif()
endif()

add_executable(quant_sev_host
    Host/main.cpp
    Host/WebSocketServer/WsUtil.cpp
    Host/WebSocketServer/TickWsServer.cpp
)
target_include_directories(quant_sev_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Host)
target_link_libraries(quant_sev_host PRIVATE quant_sev_core)
target_compile_definitions(quant_sev_host PRIVATE QUANT_SEV_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")

if(WIN32)
    target_link_libraries(quant_sev_host PRIVATE ws2_32)
endif()

set_target_properties(quant_sev_host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

if(MSVC AND CMAKE_CONFIGURATION_TYPES)
    foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
        string(TOUPPER ${cfg} cfg_upper)
        set_target_properties(quant_sev_host PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${cfg_upper} ${CMAKE_BINARY_DIR}/bin/${cfg}
        )
    endforeach()
endif()

if(WIN32 AND QUANT_SEV_ENABLE_CTP AND QUANT_SEV_MD_LIB AND QUANT_SEV_TD_LIB)
    add_custom_command(TARGET quant_sev_host POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QUANT_SEV_CTP_LIB_DIR}/thostmduserapi_se.dll"
            "$<TARGET_FILE_DIR:quant_sev_host>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QUANT_SEV_CTP_LIB_DIR}/thosttraderapi_se.dll"
            "$<TARGET_FILE_DIR:quant_sev_host>"
        COMMENT "Copying CTP runtime DLLs"
    )
endif()

add_custom_command(TARGET quant_sev_host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:quant_sev_host>"
        "$<TARGET_FILE_DIR:quant_sev_host>/Quant_Sev.exe"
    COMMENT "Sync Quant_Sev.exe alias for legacy launchers"
)
