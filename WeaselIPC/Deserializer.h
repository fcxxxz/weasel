#pragma once
#include <ResponseParser.h>
#include <functional>

namespace weasel {

template <typename T>
void TryDeserialize(std::wstringstream& ss,
                    T& t,
                    ResponseParser* target = nullptr) {
  try {
    // The archive constructor itself reads and validates the signature, so
    // it must stay inside the guard: a malformed payload would otherwise
    // escape as an uncaught exception and abort the host process.
    boost::archive::text_wiarchive ia(ss);
    ia >> t;
  } catch (...) {
    // A malformed or truncated payload must neither crash the host process
    // with an escaped exception (e.g. bad_alloc from a garbage element count)
    // nor block its UI thread on a modal message box. Reset the target so
    // partially deserialized data is never consumed as valid state.
    try {
      t = T();
    } catch (...) {
    }
    if (target)
      target->MarkMalformed();
    OutputDebugStringA("weasel: IPC payload deserialization failed\n");
  }
}
class Deserializer {
 public:
  typedef std::vector<std::wstring> KeyType;
  typedef std::shared_ptr<Deserializer> Ptr;
  typedef std::function<Ptr(ResponseParser* pTarget)> Factory;

  Deserializer(ResponseParser* pTarget) : m_pTarget(pTarget) {}
  virtual ~Deserializer() {}
  virtual void Store(KeyType const& key, std::wstring const& value) {}

  static void Initialize(ResponseParser* pTarget);
  static void Define(std::wstring const& action, Factory factory);
  static bool Require(std::wstring const& action, ResponseParser* pTarget);

 protected:
  ResponseParser* m_pTarget;

 private:
  static std::map<std::wstring, Factory> s_factories;
};

}  // namespace weasel
