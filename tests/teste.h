// Harness mínimo dos testes C++ do Pestana Estúdio (sem bibliotecas externas).
// Uso: TESTE(nome) { CONFERE(cond); CONFERE_PERTO(a, b, tol); }  e  int main() { return rodarTestes(); }
// Cada executável retorna 0 se tudo passou e 1 se algo falhou, imprimindo o que falhou.
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace teste {

struct Caso { const char* nome; std::function<void()> corpo; };
inline std::vector<Caso>& casos() { static std::vector<Caso> v; return v; }
inline int& falhasDoCaso() { static int f = 0; return f; }

struct Registro {
  Registro(const char* nome, std::function<void()> corpo) { casos().push_back({nome, std::move(corpo)}); }
};

inline void falhou(const char* arquivo, int linha, const std::string& msg) {
  std::printf("    FALHOU %s:%d: %s\n", arquivo, linha, msg.c_str());
  falhasDoCaso()++;
}

/// Informação extra (números medidos) que aparece no log.
inline void info(const char* fmt, double a = 0, double b = 0, double c = 0) {
  std::printf("    ");
  std::printf(fmt, a, b, c);
  std::printf("\n");
}

} // namespace teste

#define TESTE_CONCAT2(a, b) a##b
#define TESTE_CONCAT(a, b) TESTE_CONCAT2(a, b)
#define TESTE(nome)                                                                   \
  static void TESTE_CONCAT(teste_, nome)();                                           \
  static teste::Registro TESTE_CONCAT(registro_, nome)(#nome, TESTE_CONCAT(teste_, nome)); \
  static void TESTE_CONCAT(teste_, nome)()

#define CONFERE(cond)                                                          \
  do {                                                                         \
    if (!(cond)) teste::falhou(__FILE__, __LINE__, "esperado: " #cond);        \
  } while (0)

#define CONFERE_MSG(cond, ...)                                                 \
  do {                                                                         \
    if (!(cond)) {                                                             \
      char buf_[512];                                                          \
      std::snprintf(buf_, sizeof buf_, __VA_ARGS__);                           \
      teste::falhou(__FILE__, __LINE__, std::string(#cond " — ") + buf_);       \
    }                                                                          \
  } while (0)

#define CONFERE_IGUAL(a, b)                                                    \
  do {                                                                         \
    const long long va_ = static_cast<long long>(a);                          \
    const long long vb_ = static_cast<long long>(b);                          \
    if (va_ != vb_) {                                                          \
      char buf_[256];                                                          \
      std::snprintf(buf_, sizeof buf_, "%s = %lld, esperado %s = %lld", #a, va_, #b, vb_); \
      teste::falhou(__FILE__, __LINE__, buf_);                                 \
    }                                                                          \
  } while (0)

#define CONFERE_PERTO(a, b, tol)                                               \
  do {                                                                         \
    const double va_ = static_cast<double>(a);                                \
    const double vb_ = static_cast<double>(b);                                \
    if (!(std::fabs(va_ - vb_) <= (tol))) {                                    \
      char buf_[256];                                                          \
      std::snprintf(buf_, sizeof buf_, "%s = %.6g, esperado %.6g (±%.3g)", #a, va_, vb_, static_cast<double>(tol)); \
      teste::falhou(__FILE__, __LINE__, buf_);                                 \
    }                                                                          \
  } while (0)

/// Variável de ambiente TESTE_FILTRO=<trecho>: roda só os testes cujo nome contém o trecho.
inline int rodarTestes() {
  int falhos = 0;
  const char* filtro = std::getenv("TESTE_FILTRO");
  for (auto& c : teste::casos()) {
    if (filtro != nullptr && *filtro != '\0' && std::string(c.nome).find(filtro) == std::string::npos) continue;
    teste::falhasDoCaso() = 0;
    std::printf("[ RODA ] %s\n", c.nome);
    std::fflush(stdout);
    c.corpo();
    if (teste::falhasDoCaso() == 0) {
      std::printf("[ PASSOU ] %s\n", c.nome);
    } else {
      std::printf("[ FALHOU ] %s (%d verificações)\n", c.nome, teste::falhasDoCaso());
      falhos++;
    }
    std::fflush(stdout);
  }
  std::printf("%d de %zu testes passaram\n", static_cast<int>(teste::casos().size()) - falhos, teste::casos().size());
  return falhos == 0 ? 0 : 1;
}
