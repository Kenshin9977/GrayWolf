#include "stdafx.h"

#include "MaterialDescription.h"

// Modern Warfare Remastered materials (MaterialDescription.h): the semantics the live
// reader names (GameModernWarfareRM::ReadXMaterial): colour, normal (BC5, x,y) and specular.
namespace
{
    using Materials::Role;
    const Materials::SemanticAdapter Adapter("mwr", {
        { 0xA0AB1041, Role::Albedo },
        { 0x59D30D0F, Role::Normal },
        { 0x34ECCCB3, Role::Specular },
    });
    const Materials::AdapterRegistration Registered("H1", Adapter);
}
