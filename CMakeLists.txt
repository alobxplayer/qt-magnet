cmake_minimum_required(VERSION 3.16)

project(qt-magnet LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network LinguistTools)

set(SOURCES
    src/main.cpp
    src/config.cpp
    src/config.h
    src/logger.cpp
    src/logger.h
    src/secretstore.h
    src/magnetlink.cpp
    src/magnetlink.h
    src/format.cpp
    src/format.h
    src/qbtclient.cpp
    src/qbtclient.h
    src/worker.cpp
    src/worker.h
    src/adddialog.cpp
    src/adddialog.h
    src/settingsdialog.cpp
    src/settingsdialog.h
    src/magnethandler.h
)

if(WIN32)
    list(APPEND SOURCES
        src/secretstore_win.cpp
        src/magnethandler_win.cpp
        resources/win/app.rc
    )
    set(PLATFORM_LIBS
        crypt32
        advapi32
        shell32
        ole32
    )
elseif(UNIX AND NOT APPLE)
    list(APPEND SOURCES
        src/secretstore_unix.cpp
        src/magnethandler_unix.cpp
    )

    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBSECRET libsecret-1 glib-2.0)
        if(LIBSECRET_FOUND)
            add_compile_definitions(HAVE_LIBSECRET)
            list(APPEND PLATFORM_LIBS ${LIBSECRET_LIBRARIES})
            set(LIBSECRET_INCLUDES ${LIBSECRET_INCLUDE_DIRS})
        endif()
    endif()
elseif(APPLE)
    list(APPEND SOURCES
        src/secretstore_unix.cpp
        src/magnethandler_unix.cpp
    )
endif()

add_executable(qt-magnet WIN32 ${SOURCES})

target_include_directories(qt-magnet PRIVATE src ${LIBSECRET_INCLUDES})

target_link_libraries(qt-magnet PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    ${PLATFORM_LIBS}
)

if(MSVC)
    target_compile_options(qt-magnet PRIVATE /utf-8)
endif()

# Translations
qt6_add_translations(qt-magnet
    TS_FILES
        translations/qt-magnet_en_US.ts
        translations/qt-magnet_ru_RU.ts
    QM_FILES_OUTPUT_VARIABLE QM_FILES
)

# Installation
install(TARGETS qt-magnet RUNTIME DESTINATION .)
install(FILES ${QM_FILES} DESTINATION translations)

# Tests
enable_testing()

set(TEST_SOURCES
    tests/smoketests.cpp
    tests/mocktorrentserver.cpp
    tests/mocktorrentserver.h
    src/config.cpp
    src/config.h
    src/logger.cpp
    src/logger.h
    src/secretstore.h
    src/magnetlink.cpp
    src/magnetlink.h
    src/format.cpp
    src/format.h
    src/qbtclient.cpp
    src/qbtclient.h
    src/worker.cpp
    src/worker.h
)

if(WIN32)
    list(APPEND TEST_SOURCES src/secretstore_win.cpp)
    set(TEST_PLATFORM_LIBS crypt32 advapi32 shell32 ole32)
elseif(UNIX AND NOT APPLE)
    list(APPEND TEST_SOURCES src/secretstore_unix.cpp)
    if(LIBSECRET_FOUND)
        list(APPEND TEST_PLATFORM_LIBS ${LIBSECRET_LIBRARIES})
    endif()
elseif(APPLE)
    list(APPEND TEST_SOURCES src/secretstore_unix.cpp)
endif()

add_executable(smoketests ${TEST_SOURCES})
target_include_directories(smoketests PRIVATE src tests ${LIBSECRET_INCLUDES})
target_link_libraries(smoketests PRIVATE
    Qt6::Core
    Qt6::Network
    ${TEST_PLATFORM_LIBS}
)
if(MSVC)
    target_compile_options(smoketests PRIVATE /utf-8)
endif()

add_test(NAME SmokeTests COMMAND smoketests)
