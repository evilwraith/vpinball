#pragma once

// Which AtGames cabinet are we running on?
//
// The RK3588 support in this fork was written for the ALP4K, whose playfield is 3840x2160. The HDP
// is the same SoC but a 1920x1080 playfield, and a few things are sized against the 4K panel rather
// than against the actual one (the in-game UI's absolute pixel metrics, and BackBufferScale, which
// only makes sense when there is headroom above the panel's native resolution).
//
// Detection is by device-tree model, matching what the standalone launcher does so the two always
// agree about which cabinet this is. An unrecognised model falls back to classifying by the
// playfield's own width, so a future cabinet behaves sensibly without a code change.
//
// Header-only on purpose: no CMake changes, and both RenderDevice.cpp and LiveUI.cpp can use it.

#include <fstream>
#include <sstream>
#include <string>

namespace VP::Machine
{

// What a window is FOR, rather than where it happens to sit. Defined unconditionally so call sites
// can name their role on every platform; only RK3588 acts on it.
//
// Aux windows used to be positioned by absolute X/Y in the combined virtual desktop, which encodes
// one cabinet's geometry: the ALP4K's backglass is at X=5040 because its playfield is 3840 wide,
// the HDP's is at X=0. Naming the role instead means a new cabinet is a row in the table below
// rather than a fresh set of magic offsets.
enum class DisplayRole
{
   NoRole,    // not role-placed - use the stored coordinates (all non-RK3588 builds)
   Playfield,
   Backglass,
   DmdPanel,  // the tall 1200x1920 DSI panel (PinMAME / FlexDMD / B2SDMD / PUP DMD + FullDMD)
   Topper,    // no physical topper on either cabinet; mapped so it lands somewhere sane
};

inline const char* GetRoleName(const DisplayRole role)
{
   switch (role)
   {
   case DisplayRole::Playfield: return "Playfield";
   case DisplayRole::Backglass: return "Backglass";
   case DisplayRole::DmdPanel: return "DmdPanel";
   case DisplayRole::Topper: return "Topper";
   default: return "None";
   }
}

} // namespace VP::Machine

#ifdef __RK3588__

namespace VP::Machine
{

enum class Class
{
   Unknown, // model not recognised - callers fall back to the panel resolution
   Alp4k,   // HA9920, 3840x2160 playfield
   Hdp,     // HA9919, 1920x1080 playfield
};

// The panel height the in-game UI's pixel metrics were authored against.
inline constexpr int UI_REFERENCE_HEIGHT = 2160;

// Playfields at least this wide are treated as the 4K class when the model is unknown.
inline constexpr int FOUR_K_WIDTH_THRESHOLD = 2560;

// Raw /proc/device-tree/model, read once. Empty when unreadable. The property is NUL-terminated
// and may carry trailing whitespace.
inline const std::string& GetModel()
{
   static const std::string model = []() -> std::string {
      std::ifstream in("/proc/device-tree/model", std::ios::binary);
      if (!in)
         return std::string();

      std::ostringstream buffer;
      buffer << in.rdbuf();
      std::string value = buffer.str();

      const size_t nul = value.find('\0');
      if (nul != std::string::npos)
         value.erase(nul);

      while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' '))
         value.pop_back();

      return value;
   }();
   return model;
}

inline Class GetClass()
{
   static const Class machineClass = []() {
      const std::string& model = GetModel();
      if (model == "HA9920")
         return Class::Alp4k;
      if (model == "HA9919")
         return Class::Hdp;
      return Class::Unknown;
   }();
   return machineClass;
}

// True when the playfield is the 4K class, i.e. when rendering below the panel's native resolution
// and letting the display controller upscale is a meaningful trade. panelWidth is consulted only
// when the model is unrecognised.
inline bool IsFourKPlayfield(const int panelWidth)
{
   switch (GetClass())
   {
   case Class::Alp4k: return true;
   case Class::Hdp: return false;
   default: return panelWidth >= FOUR_K_WIDTH_THRESHOLD;
   }
}

inline int GetRoleDisplayIndex(const DisplayRole role)
{
   switch (GetClass())
   {
   case Class::Alp4k:
      // display 0: 3840x2160 playfield, 1: 1200x1920 DSI panel, 2: 1920x1080 backglass
      switch (role)
      {
      case DisplayRole::Playfield: return 0;
      case DisplayRole::DmdPanel: return 1;
      case DisplayRole::Topper: return 1;
      case DisplayRole::Backglass: return 2;
      default: return -1;
      }
   case Class::Hdp:
      // display 0: 1920x1080 backglass, 1: 1200x1920 DSI panel, 2: 1920x1080 playfield
      switch (role)
      {
      case DisplayRole::Backglass: return 0;
      case DisplayRole::DmdPanel: return 1;
      case DisplayRole::Topper: return 1;
      case DisplayRole::Playfield: return 2;
      default: return -1;
      }
   default:
      return -1;
   }
}

// Default for Player/Display when the table never set one. The historical default (0, or -1 meaning
// "primary") is the playfield on the ALP4K but the BACKGLASS on the HDP, so an unconfigured table
// renders the playfield on the wrong panel there. Returns fallback when this cabinet is unknown.
inline int GetDefaultPlayfieldDisplay(const int fallback)
{
   const int index = GetRoleDisplayIndex(DisplayRole::Playfield);
   return index >= 0 ? index : fallback;
}

// For logging: "ALP4K (HA9920)", "HDP (HA9919)", or "unknown model '<x>'".
inline std::string Describe()
{
   switch (GetClass())
   {
   case Class::Alp4k: return "ALP4K (HA9920)";
   case Class::Hdp: return "HDP (HA9919)";
   default: return "unknown model '" + GetModel() + '\'';
   }
}

} // namespace VP::Machine

#endif // __RK3588__
