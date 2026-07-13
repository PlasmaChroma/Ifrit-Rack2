#include "ifrit_plugin.hpp"

Plugin* pluginInstance;

void init(Plugin* p) {
    pluginInstance = p;

    // Add modules here
    p->addModel(modelVcvDoom);
    p->addModel(modelVstFx);
    p->addModel(modelVstInstrument);
}
