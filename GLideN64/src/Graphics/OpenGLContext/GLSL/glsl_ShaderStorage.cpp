#include <fstream>
#include <assert.h>
#include <cstdlib>
#include <algorithm>
#include <iomanip>

#include <Graphics/CombinerProgram.h>
#include <Graphics/Context.h>
#include <Graphics/OpenGLContext/opengl_Utils.h>
#include <Types.h>
#include <Log.h>
#include <RSP.h>
#include <PluginAPI.h>
#include <Combiner.h>
#include "glsl_Utils.h"
#include "glsl_ShaderStorage.h"
#include "glsl_CombinerProgramImpl.h"
#include "glsl_CombinerProgramUniformFactory.h"

using namespace glsl;

#define SHADER_STORAGE_FOLDER_NAME "shaders"

/*
Storage has text format:
line_1 Version in hex form
line_2 Hardware per pixel lighting support flag
line_3 Count - numbers of combiners keys in hex form
line_4..line_Count+3  combiners keys in hex form, one key per line
*/
bool ShaderStorage::_saveCombinerKeys(const graphics::Combiners & _combiners) const
{
	return false;
}

/*
Storage format:
uint32 - format version;
uint32 - bitset of config options, which may change how shader is created.
uint32 - len of renderer string
char * - renderer string
uint32 - len of GL version string
char * - GL version string
uint32 - number of shaders
shaders in binary form
*/
bool ShaderStorage::saveShadersStorage(const graphics::Combiners & _combiners) const
{
	return false;
}

static
CombinerProgramImpl * _readCominerProgramFromStream(std::istream & _is,
	CombinerProgramUniformFactory & _uniformFactory,
	opengl::CachedUseProgram * _useProgram)
{
	CombinerKey cmbKey;
	cmbKey.read(_is);

	int inputs;
	_is.read((char*)&inputs, sizeof(inputs));
	CombinerInputs cmbInputs(inputs);

	GLenum binaryFormat;
	GLint  binaryLength;
	_is.read((char*)&binaryFormat, sizeof(binaryFormat));
	_is.read((char*)&binaryLength, sizeof(binaryLength));
	std::vector<char> binary(binaryLength);
	_is.read(binary.data(), binaryLength);

	GLuint program = glCreateProgram();
	const bool isRect = cmbKey.isRectKey();
	glsl::Utils::locateAttributes(program, isRect, cmbInputs.usesTexture());
	glProgramBinary(program, binaryFormat, binary.data(), binaryLength);
	assert(glsl::Utils::checkProgramLinkStatus(program));

	UniformGroups uniforms;
	_uniformFactory.buildUniforms(program, cmbInputs, cmbKey, uniforms);

	return new CombinerProgramImpl(cmbKey, program, _useProgram, cmbInputs, std::move(uniforms));
}

bool ShaderStorage::_loadFromCombinerKeys(graphics::Combiners & _combiners)
{
	return false;
}

bool ShaderStorage::loadShadersStorage(graphics::Combiners & _combiners)
{
	return false;
}


ShaderStorage::ShaderStorage(const opengl::GLInfo & _glinfo, opengl::CachedUseProgram * _useProgram)
: m_glinfo(_glinfo)
, m_useProgram(_useProgram)
{
}
