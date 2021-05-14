//
// Copyright 2020 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
#include "pxr/imaging/hdSt/materialXFilter.h"
#include "pxr/imaging/hdSt/materialXShaderGen.h"
#include "pxr/imaging/hdSt/tokens.h"
#include "pxr/imaging/hdMtlx/hdMtlx.h"

#include "pxr/usd/sdr/registry.h"
#include "pxr/imaging/hio/glslfx.h"

#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/matrix3d.h"
#include "pxr/base/gf/matrix4d.h"

#include "pxr/base/tf/diagnostic.h"

#include <MaterialXGenShader/Util.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXRender/Util.h>
#include <MaterialXRender/LightHandler.h> 

namespace mx = MaterialX;

PXR_NAMESPACE_OPEN_SCOPE


TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    (mtlx)

    // Default Texture Coordinate Token
    (st)
    (texcoord)
    (geomprop)
);


////////////////////////////////////////////////////////////////////////////////
// Shader Gen Functions

// Generate the Glsl Pixel Shader based on the given mxContext and mxElement
// Based on MaterialXViewer Material::generateShader()
static std::string
_GenPixelShader(mx::GenContext & mxContext, 
                mx::ElementPtr const& mxElem)
{
    bool hasTransparency = mx::isTransparentSurface(mxElem, 
                                mxContext.getShaderGenerator());

    mx::GenContext materialContext = mxContext;
    materialContext.getOptions().hwTransparency = hasTransparency;
    materialContext.getOptions().hwShadowMap = 
        materialContext.getOptions().hwShadowMap && !hasTransparency;
    
    // Use the domeLightPrefilter texture instead of sampling the Environment Map
    materialContext.getOptions().hwSpecularEnvironmentMethod =
        mx::HwSpecularEnvironmentMethod::SPECULAR_ENVIRONMENT_PREFILTER;

    mx::ShaderPtr mxShader = mx::createShader("Shader", materialContext, mxElem);
    if (mxShader) {
        return mxShader->getSourceCode(mx::Stage::PIXEL);
    }
    return mx::EMPTY_STRING;
}

// Results in lightData.type = 1 for point lights in the Mx Shader
static const std::string mxDirectLightString = 
R"(
<?xml version="1.0"?>
<materialx version="1.37">
  <point_light name="pt_light" type="lightshader">
  </point_light>
</materialx>
)";

// Use the given mxDocument to generate the corresponding glsl source code
// Based on MaterialXViewer Viewer::loadDocument()
std::string
HdSt_GenMaterialXShaderCode(
    mx::DocumentPtr const& mxDoc,
    mx::FileSearchPath const& searchPath,
    mx::StringMap const& mxHdTextureMap,
    mx::StringMap const& mxHdPrimvarMap,
    std::string const& defaultTexcoordName)
{
    // Initialize the Context for shaderGen. 
    mx::GenContext mxContext = HdStMaterialXShaderGen::create(mxHdTextureMap, 
                                    mxHdPrimvarMap, defaultTexcoordName);
    mxContext.registerSourceCodeSearchPath(searchPath);
    
    // Add the Direct Light mtlx file to the mxDoc 
    mx::DocumentPtr lightDoc = mx::createDocument();
    mx::readFromXmlString(lightDoc, mxDirectLightString);
    mxDoc->importLibrary(lightDoc);

    // Make sure the Light data properties are added to the mxLightData struct
    mx::LightHandler lightHandler;
    std::vector<mx::NodePtr> lights;
    lightHandler.findLights(mxDoc, lights);
    lightHandler.registerLights(mxDoc, lights, mxContext);

    // Find renderable elements in the Mtlx Document.
    std::vector<mx::TypedElementPtr> renderableElements;
    mx::findRenderableElements(mxDoc, renderableElements);

    // Should have exactly one renderable element (material).
    if (renderableElements.size() != 1) {
        TF_CODING_ERROR("Generated MaterialX Document does not "
                        "have 1 material");
        return mx::EMPTY_STRING;
    }

    // Extract out the Surface Shader Node for the Material Node 
    mx::TypedElementPtr renderableElem = renderableElements.at(0);
    mx::NodePtr node = renderableElem->asA<mx::Node>();
    if (node && node->getType() == mx::MATERIAL_TYPE_STRING) {
        std::unordered_set<mx::NodePtr> mxShaderNodes;
        mxShaderNodes = mx::getShaderNodes(node, mx::SURFACE_SHADER_TYPE_STRING);
        if (!mxShaderNodes.empty()) {
            renderableElem = *mxShaderNodes.begin();
        }
    }
    // Generate the PixelShader for the renderable element (surfaceshader).
    const mx::ElementPtr & mxElem = mxDoc->getDescendant(
                                            renderableElem->getNamePath());
    mx::TypedElementPtr typedElem = mxElem ? mxElem->asA<mx::TypedElement>()
                                         : nullptr;
    if (typedElem) {
        return _GenPixelShader(mxContext, typedElem);
    }
    return mx::EMPTY_STRING;
}


////////////////////////////////////////////////////////////////////////////////
// Helper Functions to convert MX texture node parameters to Hd parameters

// Get the Hydra VtValue for the given MaterialX input value
static VtValue
_GetHdFilterValue(std::string const& mxInputValue)
{
    if(mxInputValue == "closest") {
        return VtValue(HdStTextureTokens->nearestMipmapNearest);
    }
    return VtValue(HdStTextureTokens->linearMipmapLinear);
}

// Get the Hydra VtValue for the given MaterialX input value
static VtValue
_GetHdSamplerValue(std::string const& mxInputValue)
{
    if (mxInputValue == "constant") {
        return VtValue(HdStTextureTokens->black);
    }
    if (mxInputValue == "clamp") {
        return VtValue(HdStTextureTokens->clamp);
    }
    if (mxInputValue == "mirror") {
        return VtValue(HdStTextureTokens->mirror);
    }
    return VtValue(HdStTextureTokens->repeat);
}

// Translate the MaterialX texture node input into the Hydra equivalents
static void
_GetHdTextureParameters(
    std::string const& mxInputName,
    std::string const& mxInputValue,
    std::map<TfToken, VtValue> * hdTextureParams)
{
    // MaterialX has two texture2d node types <image> and <tiledimage>

    // Properties common to both <image> and <tiledimage> texture nodes:
    if (mxInputName == "filtertype") {
        (*hdTextureParams)[HdStTextureTokens->minFilter] = 
            _GetHdFilterValue(mxInputValue);
        (*hdTextureParams)[HdStTextureTokens->magFilter] = 
            VtValue(HdStTextureTokens->linear);
    }

    // Properties specific to <image> nodes:
    else if (mxInputName == "uaddressmode") {
        (*hdTextureParams)[HdStTextureTokens->wrapS] = 
            _GetHdSamplerValue(mxInputValue);
    }
    else if (mxInputName == "vaddressmode") {
        (*hdTextureParams)[HdStTextureTokens->wrapT] = 
            _GetHdSamplerValue(mxInputValue);
    }

    // Properties specific to <tiledimage> nodes:
    else if (mxInputName == "uvtiling" || mxInputName == "uvoffset" ||
        mxInputName == "realworldimagesize" || 
        mxInputName == "realworldtilesize") {
        (*hdTextureParams)[HdStTextureTokens->wrapS] = 
            VtValue(HdStTextureTokens->repeat);
        (*hdTextureParams)[HdStTextureTokens->wrapT] = 
            VtValue(HdStTextureTokens->repeat);
    }
}

// Find the HdNode and its corresponding NodePath in the given HdNetwork 
// based on the given HdConnection
static bool 
_FindConnectedNode(
    HdMaterialNetwork2 const& hdNetwork,
    HdMaterialConnection2 const& hdConnection,
    HdMaterialNode2 * hdNode,
    SdfPath * hdNodePath)
{
    // Get the path to the connected node
    const SdfPath & connectionPath = hdConnection.upstreamNode;

    // If this path is not in the network raise a warning
    auto hdNodeIt = hdNetwork.nodes.find(connectionPath);
    if (hdNodeIt == hdNetwork.nodes.end()) {
        TF_WARN("Unknown material node '%s'", connectionPath.GetText());
        return false;
    }

    // Otherwise return the HdNode and corresponding NodePath
    *hdNode = hdNodeIt->second;
    *hdNodePath = connectionPath;
    return true;
}

// Get the Texture coordinate name if specified, otherwise get the default name 
static void
_GetTextureCoordinateName(
    HdMaterialNetwork2 * hdNetwork,
    HdMaterialNode2 * hdTextureNode,
    SdfPath const& hdTextureNodePath,
    mx::StringMap * mxHdPrimvarMap,
    std::string * defaultTexcoordName)
{
    // Get the Texture Coordinate name through the connected node
    bool textureCoordSet = false;
    for (auto& inputConnections : hdTextureNode->inputConnections) {

        // Texture Coordinates are connected through the 'texcoord' input
        if ( inputConnections.first != _tokens->texcoord) {
            continue;
        }

        for (auto& currConnection : inputConnections.second) {

            // Get the connected Texture Coordinate node
            SdfPath hdCoordNodePath;
            HdMaterialNode2 hdCoordNode;
            const bool found = _FindConnectedNode(*hdNetwork, currConnection,
                                            &hdCoordNode, &hdCoordNodePath);
            if (!found) {
                continue;
            }
            
            // Get the texture coordinate name from the 'geomprop' parameter
            auto coordNameIt = hdCoordNode.parameters.find(_tokens->geomprop);
            if (coordNameIt != hdCoordNode.parameters.end()) {

                std::string const& texcoordName = 
                    HdMtlxConvertToString(coordNameIt->second);
                
                // Set the 'st' parameter as a TfToken
                // hdTextureNode->parameters[_tokens->st] = 
                hdNetwork->nodes[hdTextureNodePath].parameters[_tokens->st] = 
                                TfToken(texcoordName.c_str());

                // Save texture coordinate primvar name for the glslfx header
                (*mxHdPrimvarMap)[texcoordName] = "vec2";
                textureCoordSet = true;
                break;
            }
        }
    }
    
    // If we did not have a connected node, and the 'st' parameter is not set
    // get the default texture cordinate name from the textureNodes sdr metadata 
    if ( !textureCoordSet && hdTextureNode->parameters.find(_tokens->st) == 
                             hdTextureNode->parameters.end()) {

        // Get the sdr node for the mxTexture node
        SdrRegistry &sdrRegistry = SdrRegistry::GetInstance();
        const SdrShaderNodeConstPtr sdrTextureNode = 
            sdrRegistry.GetShaderNodeByIdentifierAndType(
                hdTextureNode->nodeTypeId, _tokens->mtlx);

        if (sdrTextureNode) {

            // Get the primvarname from the sdrTextureNode metadata
            auto metadata = sdrTextureNode->GetMetadata();
            auto primvarName = metadata[SdrNodeMetadata->Primvars];

            // Set the 'st' parameter as a TfToken
            hdNetwork->nodes[hdTextureNodePath].parameters[_tokens->st] = 
                            TfToken(primvarName.c_str());

            // Save the default texture coordinate name for the glslfx header
            *defaultTexcoordName = primvarName;
        }
        
    }
}

// Add the Hydra texture node parameters to the texture nodes
static void 
_AddHdTextureNodeParameters(
    HdMaterialNetwork2 * hdNetwork,
    std::set<SdfPath> const& hdTextureNodes,
    mx::StringMap * mxHdPrimvarMap,
    std::string * defaultTexcoordName)
{
    for (auto const& texturePath : hdTextureNodes) {
        HdMaterialNode2 hdTextureNode = hdNetwork->nodes[texturePath];

        _GetTextureCoordinateName(hdNetwork, &hdTextureNode, texturePath, 
                                  mxHdPrimvarMap, defaultTexcoordName);

        // Gather the Hydra Texture Parameters
        std::map<TfToken, VtValue> hdParameters;
        for (auto const& currParam : hdTextureNode.parameters) {
        
            // Get the MaterialX Input Value string
            std::string mxInputValue = HdMtlxConvertToString(currParam.second);

            // Get the Hydra equivalents for the MX Texture node parameters
            std::string mxInputName = currParam.first.GetText();
            _GetHdTextureParameters(mxInputName, mxInputValue, &hdParameters);
        }

        // Add the Hydra Texture Parameters to the Texture Node
        for (auto param : hdParameters) {
            hdNetwork->nodes[texturePath].parameters[param.first] = param.second;
        }
    }
}


void
HdSt_ApplyMaterialXFilter(
    HdMaterialNetwork2 * hdNetwork,
    SdfPath const& materialPath,
    HdMaterialNode2 const& terminalNode,
    SdfPath const& terminalNodePath)
{
    // Check if the Terminal is a MaterialX Node
    SdrRegistry &sdrRegistry = SdrRegistry::GetInstance();
    const SdrShaderNodeConstPtr mtlxSdrNode = 
        sdrRegistry.GetShaderNodeByIdentifierAndType(terminalNode.nodeTypeId, 
                                                     _tokens->mtlx);

    if (mtlxSdrNode) {

        // Load Standard Libraries/setup SearchPaths (for mxDoc and mxShaderGen)
        mx::FilePathVec libraryFolders = { "libraries", };
        mx::FileSearchPath searchPath;
        searchPath.append(mx::FilePath(PXR_MATERIALX_STDLIB_DIR));
        searchPath.append(mx::FilePath(PXR_MATERIALX_BASE_DIR));
        mx::DocumentPtr stdLibraries = mx::createDocument();
        mx::loadLibraries(libraryFolders, searchPath, stdLibraries);

        // Create the MaterialX Document from the HdMaterialNetwork
        std::set<SdfPath> hdTextureNodes;
        mx::StringMap mxHdTextureMap; // Mx-Hd texture name counterparts 
        mx::DocumentPtr mtlxDoc = HdMtlxCreateMtlxDocumentFromHdNetwork(
                                        *hdNetwork,
                                        terminalNode,   // MaterialX HdNode
                                        materialPath,
                                        stdLibraries,
                                        &hdTextureNodes,
                                        &mxHdTextureMap);

        // Add Hydra parameters for each of the Texture nodes
        mx::StringMap mxHdPrimvarMap; // Primvar name and type
        std::string defaultTexcoordName;
        _AddHdTextureNodeParameters(hdNetwork, hdTextureNodes, 
                                    &mxHdPrimvarMap, &defaultTexcoordName);

        // Load MaterialX Document and generate the glslfxSource
        std::string glslfxSource = HdSt_GenMaterialXShaderCode(mtlxDoc, 
                                    searchPath, mxHdTextureMap, 
                                    mxHdPrimvarMap, defaultTexcoordName);

        // Create a new terminal node with the new glslfxSource
        SdrShaderNodeConstPtr sdrNode = 
            sdrRegistry.GetShaderNodeFromSourceCode(glslfxSource, 
                                                    HioGlslfxTokens->glslfx,
                                                    NdrTokenMap()); // metadata
        HdMaterialNode2 newTerminalNode;
        newTerminalNode.nodeTypeId = sdrNode->GetIdentifier();
        newTerminalNode.inputConnections = terminalNode.inputConnections;

        // Replace the original terminalNode with this newTerminalNode
        hdNetwork->nodes[terminalNodePath] = newTerminalNode;
    }
}

PXR_NAMESPACE_CLOSE_SCOPE