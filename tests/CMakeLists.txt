# Testes C++ do núcleo de áudio (Core/DSP) do Pestana Estúdio.
#   cmake -S tests -B build/testes -DPESTANA_SANITIZE=ON && cmake --build build/testes -j && ctest --test-dir build/testes --output-on-failure
# Opções:
#   PESTANA_SANITIZE=ON  AddressSanitizer + UndefinedBehaviorSanitizer (use g++)
#   PESTANA_TSAN=ON      ThreadSanitizer (não junta com PESTANA_SANITIZE)
cmake_minimum_required(VERSION 3.16)
project(PestanaTestes LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(PESTANA_SANITIZE "AddressSanitizer + UBSan" OFF)
option(PESTANA_TSAN "ThreadSanitizer" OFF)
if(PESTANA_SANITIZE AND PESTANA_TSAN)
  message(FATAL_ERROR "PESTANA_SANITIZE e PESTANA_TSAN não podem ser ligados juntos")
endif()

set(RAIZ ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(DSP_LOOPER ${RAIZ}/Afinador/Looper)
set(DSP_NOISE ${RAIZ}/Afinador/Noise)
set(DSP_GUITAR ${RAIZ}/Afinador/GuitarFX)
set(VENDOR ${RAIZ}/Vendor)

if(PESTANA_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()
if(PESTANA_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)

# ---- núcleo (código de produção, sem mudança)
add_library(pestana_looper STATIC ${DSP_LOOPER}/LooperCore.cpp)
target_include_directories(pestana_looper PUBLIC ${DSP_LOOPER})
target_compile_options(pestana_looper PRIVATE -Wall -Wextra)

add_library(pestana_noise STATIC ${DSP_NOISE}/NoiseReducer.cpp)
target_include_directories(pestana_noise PUBLIC ${DSP_NOISE})
target_compile_options(pestana_noise PRIVATE -Wall -Wextra)

file(GLOB NAM_FONTES ${VENDOR}/NAM/*.cpp ${VENDOR}/NAM/wavenet/*.cpp)
add_library(pestana_nam STATIC ${NAM_FONTES})
target_include_directories(pestana_nam SYSTEM PUBLIC ${VENDOR} ${VENDOR}/NAM ${VENDOR}/eigen ${VENDOR}/nlohmann)
target_compile_definitions(pestana_nam PUBLIC NAM_SAMPLE_FLOAT=1 NAM_ENABLE_A2_FAST=1)
target_compile_options(pestana_nam PRIVATE -w)

add_library(pestana_guitar STATIC ${DSP_GUITAR}/GuitarFXCore.cpp)
target_include_directories(pestana_guitar PUBLIC ${DSP_GUITAR})
target_link_libraries(pestana_guitar PUBLIC pestana_nam)
target_compile_options(pestana_guitar PRIVATE -Wall -Wextra)

# ---- testes
enable_testing()
function(pestana_teste nome biblioteca)
  add_executable(${nome} ${nome}.cpp)
  target_include_directories(${nome} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(${nome} PRIVATE -Wall -Wextra)
  target_link_libraries(${nome} PRIVATE ${biblioteca} Threads::Threads)
  add_test(NAME ${nome} COMMAND ${nome})
endfunction()

pestana_teste(looper_manual pestana_looper)
pestana_teste(looper_auto pestana_looper)
pestana_teste(looper_camadas pestana_looper)
pestana_teste(looper_estresse pestana_looper)
pestana_teste(looper_defeitos pestana_looper)
pestana_teste(noise_reducer pestana_noise)
pestana_teste(guitarfx_drive pestana_guitar)

set_tests_properties(looper_defeitos PROPERTIES LABELS "defeitos")
set_tests_properties(looper_estresse PROPERTIES TIMEOUT 120)
