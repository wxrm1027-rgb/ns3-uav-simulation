#include "state_manager.h"

#include <sstream>

namespace hnms
{

GlobalStateManager&
GlobalStateManager::Instance ()
{
  static GlobalStateManager inst;
  return inst;
}

void
GlobalStateManager::UpdateState (const std::string& key, const std::string& valueJson)
{
  std::lock_guard<std::mutex> lock (m_mutex);
  m_kv[key] = valueJson;
  for (const auto& s : m_subs)
    {
      const std::string& p = s.second.prefix;
      if (key.compare (0, p.size (), p) == 0)
        {
          s.second.cb (key, valueJson);
        }
    }
}

GlobalStateManager::SubscriptionId
GlobalStateManager::Subscribe (
    const std::string& keyPrefix,
    std::function<void (const std::string& key, const std::string& value)> cb)
{
  std::lock_guard<std::mutex> lock (m_mutex);
  SubscriptionId id = m_nextId++;
  m_subs[id] = Sub{keyPrefix, std::move (cb)};
  return id;
}

bool
GlobalStateManager::Unsubscribe (SubscriptionId id)
{
  std::lock_guard<std::mutex> lock (m_mutex);
  return m_subs.erase (id) > 0;
}

void
GlobalStateManager::RemoveNode (std::uint32_t nodeId)
{
  std::ostringstream prefix;
  prefix << "node/" << nodeId << "/";
  const std::string pre = prefix.str ();
  std::lock_guard<std::mutex> lock (m_mutex);
  std::vector<std::string> keys;
  for (const auto& kv : m_kv)
    {
      if (kv.first.compare (0, pre.size (), pre) == 0)
        {
          keys.push_back (kv.first);
        }
    }
  const std::string nullv = "null";
  for (const auto& k : keys)
    {
      m_kv.erase (k);
      for (const auto& s : m_subs)
        {
          if (k.compare (0, s.second.prefix.size (), s.second.prefix) == 0)
            {
              s.second.cb (k, nullv);
            }
        }
    }
}

std::string
GlobalStateManager::GetState (const std::string& key) const
{
  std::lock_guard<std::mutex> lock (m_mutex);
  auto it = m_kv.find (key);
  if (it == m_kv.end ())
    {
      return {};
    }
  return it->second;
}

} // namespace hnms
