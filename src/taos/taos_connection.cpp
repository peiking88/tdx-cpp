// TDengine RAII 封装实现。
#include "tdx/taos/taos_connection.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace tdx::taos {

TaosConfig TaosConfig::FromEnv() {
  TaosConfig cfg;
  auto read = [](const char* key, std::string& out) {
    const char* v = std::getenv(key);
    if (v && *v) out = v;
  };
  read("TDX_TAOS_HOST", cfg.host);
  const char* p = std::getenv("TDX_TAOS_PORT");
  if (p && *p) cfg.port = static_cast<uint16_t>(std::atoi(p));
  read("TDX_TAOS_USER", cfg.user);
  read("TDX_TAOS_PASS", cfg.pass);
  read("TDX_TAOS_DB", cfg.db);
  return cfg;
}

bool ExecSQL(TAOS* conn, const char* sql) {
  if (!conn) return false;
  TAOS_RES* res = ::taos_query(conn, sql);
  int code = ::taos_errno(res);
  if (code != 0) {
    std::fprintf(stderr, "TDengine SQL error [%d]: %s\n  SQL: %s\n",
                 code, ::taos_errstr(res), sql);
    ::taos_free_result(res);
    return false;
  }
  ::taos_free_result(res);
  return true;
}

namespace {
  std::once_flag g_taos_init;
}

TaosConnection::TaosConnection(const TaosConfig& cfg) {
  std::call_once(g_taos_init, [] { ::taos_init(); });
  conn_ = ::taos_connect(cfg.host.c_str(), cfg.user.c_str(),
                         cfg.pass.c_str(), cfg.db.c_str(), cfg.port);
  if (!conn_) {
    std::fprintf(stderr, "TDengine 连接失败: %s:%d (user=%s)\n",
                 cfg.host.c_str(), cfg.port, cfg.user.c_str());
  }
}

TaosConnection::~TaosConnection() {
  if (conn_) ::taos_close(conn_);
}

const char* TaosConnection::errstr() const {
  return ::taos_errstr(nullptr);
}

TaosStmt::TaosStmt(TAOS* conn) {
  if (conn) stmt_ = ::taos_stmt_init(conn);
}

TaosStmt::~TaosStmt() {
  if (stmt_) ::taos_stmt_close(stmt_);
}

bool TaosStmt::Prepare(const char* sql) {
  if (!stmt_) return false;
  int rc = ::taos_stmt_prepare(stmt_, sql, static_cast<unsigned long>(std::strlen(sql)));
  if (rc != 0) {
    std::fprintf(stderr, "TDengine stmt prepare error: %s\n  SQL: %s\n", errstr(), sql);
    return false;
  }
  return true;
}

bool TaosStmt::SetTbNameTags(const char* name, TAOS_MULTI_BIND* tags) {
  if (!stmt_) return false;
  int rc = ::taos_stmt_set_tbname_tags(stmt_, name, tags);
  if (rc != 0) {
    std::fprintf(stderr, "TDengine stmt set_tbname_tags error: %s\n", errstr());
    return false;
  }
  return true;
}

bool TaosStmt::BindParamBatch(TAOS_MULTI_BIND* binds) {
  if (!stmt_) return false;
  int rc = ::taos_stmt_bind_param_batch(stmt_, binds);
  if (rc != 0) {
    std::fprintf(stderr, "TDengine stmt bind error: %s\n", errstr());
    return false;
  }
  return true;
}

bool TaosStmt::BindSingleParamBatch(TAOS_MULTI_BIND* bind, int col_idx) {
  if (!stmt_) return false;
  int rc = ::taos_stmt_bind_single_param_batch(stmt_, bind, col_idx);
  if (rc != 0) {
    std::fprintf(stderr, "TDengine stmt bind_single[%d] error: %s\n", col_idx, errstr());
    return false;
  }
  return true;
}

bool TaosStmt::AddBatch() {
  if (!stmt_) return false;
  int rc = ::taos_stmt_add_batch(stmt_);
  if (rc != 0) {
    std::fprintf(stderr, "TDengine stmt add_batch error: %s\n", errstr());
    return false;
  }
  return true;
}

bool TaosStmt::Execute(int* affected) {
  if (!stmt_) return false;
  int rc = ::taos_stmt_execute(stmt_);
  if (rc != 0) {
    std::fprintf(stderr, "TDengine stmt execute error: %s\n", errstr());
    return false;
  }
  if (affected) *affected = ::taos_stmt_affected_rows(stmt_);
  return true;
}

const char* TaosStmt::errstr() const {
  return stmt_ ? ::taos_stmt_errstr(const_cast<TAOS_STMT*>(stmt_)) : "null stmt";
}

}  // namespace tdx::taos
