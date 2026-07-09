// TDengine 连接与 STMT RAII 封装。tdx_taos 模块底层。
// 线程安全：每线程独立 TaosConnection + TaosStmt。
#pragma once

#include <memory>
#include <string>

#include <taos.h>

namespace tdx::taos {

struct TaosConfig {
  std::string host = "localhost";
  uint16_t port = 6030;
  std::string user = "root";
  std::string pass = "taosdata";
  std::string db   = "tdx";

  static TaosConfig FromEnv();
};

// exec SQL (DDL), return true on success; print error on failure.
bool ExecSQL(TAOS* conn, const char* sql);

// TDengine VARCHAR 字段读取：row[col] 指向数据，2字节 LE 长度前缀在 p[-2]/p[-1]。
inline std::string ReadVarChar(void* col) {
  if (!col) return {};
  const auto* p = static_cast<const unsigned char*>(col);
  uint16_t len = p[-2] | (static_cast<uint16_t>(p[-1]) << 8);
  return std::string(reinterpret_cast<const char*>(p), len);
}

class TaosConnection {
 public:
  explicit TaosConnection(const TaosConfig& cfg);
  ~TaosConnection();

  TaosConnection(const TaosConnection&) = delete;
  TaosConnection& operator=(const TaosConnection&) = delete;
  TaosConnection(TaosConnection&& o) noexcept : conn_(o.conn_) { o.conn_ = nullptr; }
  TaosConnection& operator=(TaosConnection&& o) noexcept {
    if (this != &o) { if (conn_) taos_close(conn_); conn_ = o.conn_; o.conn_ = nullptr; }
    return *this;
  }

  TAOS* native() const { return conn_; }
  explicit operator bool() const { return conn_ != nullptr; }
  const char* errstr() const;

 private:
  TAOS* conn_ = nullptr;
};

// 连接 tdx 库（FromEnv 配置 + USE tdx），失败返回 nullptr。
std::unique_ptr<TaosConnection> ConnectTdx();

class TaosStmt {
 public:
  explicit TaosStmt(TAOS* conn);
  ~TaosStmt();

  TaosStmt(const TaosStmt&) = delete;
  TaosStmt& operator=(const TaosStmt&) = delete;
  TaosStmt(TaosStmt&& o) noexcept : stmt_(o.stmt_) { o.stmt_ = nullptr; }
  TaosStmt& operator=(TaosStmt&& o) noexcept {
    if (this != &o) { if (stmt_) taos_stmt_close(stmt_); stmt_ = o.stmt_; o.stmt_ = nullptr; }
    return *this;
  }

  bool Prepare(const char* sql);
  bool SetTbNameTags(const char* name, TAOS_MULTI_BIND* tags);
  bool BindParamBatch(TAOS_MULTI_BIND* binds);
  bool BindSingleParamBatch(TAOS_MULTI_BIND* bind, int col_idx);
  bool AddBatch();
  bool Execute(int* affected = nullptr);
  const char* errstr() const;

  TAOS_STMT* native() const { return stmt_; }

 private:
  TAOS_STMT* stmt_ = nullptr;
};

}  // namespace tdx::taos
