/* Copyright (c) 2007 Scott Lembcke
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <limits.h>
#include <string.h>

#include "Photon.h"

#include "chipmunk/chipmunk_private.h"
#include "ChipmunkDebugDraw.h"

#include "VeraMoBd.ttf_sdf.h"


#define TextScale 0.70f
#define TextLineHeight (18.0f*TextScale)

float ChipmunkDebugDrawScaleFactor = 1.0f;
cpTransform ChipmunkDebugDrawProjection = {1, 0, 0, 1, 0, 0};
cpTransform ChipmunkDebugDrawCamera = {1, 0, 0, 1, 0, 0};

cpVect ChipmunkDebugDrawLightPosition;
cpFloat ChipmunkDebugDrawLightRadius;

static PhotonRenderer *Renderer = NULL;
static PhotonRenderState *PrimitiveState = NULL;
static PhotonRenderState *FontState = NULL;

static PhotonShader *ShadowMaskShader = NULL;
static PhotonRenderState *ShadowMaskState = NULL;
static PhotonRenderState *ShadowApplyState = NULL;

// char -> glyph indexes generated by the lonesock tool.
static int glyph_indexes[256];

static const char *DirectVShader = PHOTON_GLSL(
	in vec4 PhotonAttributePosition;
	in vec4 PhotonAttributeColor;
	
	out vec4 PhotonFragColor;
	
	void main(void){
		gl_Position = PhotonAttributePosition;
		PhotonFragColor = PhotonAttributeColor*PhotonAttributeColor.a;
	}
);

static const char *DirectFShader = PHOTON_GLSL(
	in vec4 PhotonFragColor;
	
	out vec4 PhotonFragOut;
	
	void main(void){
		PhotonFragOut = PhotonFragColor;
	}
);

static const char *PrimitiveVShader = PHOTON_GLSL(
	in vec4 PhotonAttributePosition;
	in vec2 PhotonAttributeUV1;
	in vec2 PhotonAttributeUV2;
	in vec4 PhotonAttributeColor;
	
	out vec2 PhotonFragUV1;
	out vec2 PhotonFragUV2;
	out vec4 PhotonFragColor;
	
	layout(std140) uniform;
	uniform PhotonGlobals {
		mat4 u_P;
		mat4 u_MVP;
		vec4 u_OutlineColor;
		float u_OutlineWidth;
	};
	
	void main(void){
		gl_Position = u_MVP*PhotonAttributePosition;
		PhotonFragUV1 = PhotonAttributeUV1;
		PhotonFragUV2 = PhotonAttributeUV2;
		PhotonFragColor = PhotonAttributeColor*PhotonAttributeColor.a;
	}
);

static const char *PrimitiveFShader = PHOTON_GLSL(
	in vec2 PhotonFragUV1;
	in vec2 PhotonFragUV2;
	in vec4 PhotonFragColor;
	
	out vec4 PhotonFragOut;
	
	layout(std140) uniform;
	uniform PhotonGlobals {
		mat4 u_P;
		mat4 u_MVP;
		vec4 u_OutlineColor;
		float u_OutlineWidth;
	};
	
	void main(void){
		float r1 = PhotonFragUV2[0];
		float r2 = PhotonFragUV2[1];
		
		float l = length(PhotonFragUV1);
		float fw = fwidth(l) + 1e-3;
		
		// Fill/outline color.
		float outlineWidth = fw*u_OutlineWidth;
		float outline = smoothstep(r1, r1 - fw, l);
		
		// Use pre-multiplied alpha.
		vec4 color = mix(u_OutlineColor, PhotonFragColor, outline);
		float mask = smoothstep(r2, r2 - fw, l);
		PhotonFragOut = color*mask;
	}
);

static const char *FontVShader = PHOTON_GLSL(
	in vec4 PhotonAttributePosition;
	in vec2 PhotonAttributeUV1;
	in vec4 PhotonAttributeColor;
	
	out vec2 PhotonFragUV1;
	out vec4 PhotonFragColor;
	
	layout(std140) uniform;
	uniform PhotonGlobals {
		mat4 u_P;
		mat4 u_MVP;
		vec4 u_OutlineColor;
		float u_OutlineWidth;
	};
	
	void main(void){
		gl_Position = u_P*PhotonAttributePosition;
		PhotonFragUV1 = PhotonAttributeUV1;
		PhotonFragColor = PhotonAttributeColor;
	}
);

static const char *FontFShader = PHOTON_GLSL(
	in vec2 PhotonFragUV1;
	in vec2 PhotonFragUV2;
	in vec4 PhotonFragColor;
	
	out vec4 PhotonFragOut;
	
	uniform sampler2D u_FontAtlas;
	
	void main(void){
		float sdf = texture(u_FontAtlas, PhotonFragUV1).r;
		float fw = 0.5*fwidth(sdf);
		float mask = smoothstep(0.5 - fw, 0.5 + fw, sdf);
		
		PhotonFragOut = PhotonFragColor*mask;
	}
);

static const char *ShadowMaskVShader = PHOTON_GLSL(
	in vec4 PhotonAttributePosition;
	in vec2 PhotonAttributeUV1;
	in vec2 PhotonAttributeUV2;
	in vec4 PhotonAttributeColor;
	
	out float v_Opacity;

	// .xy is one penumbra edge, .zw is the other.
	out vec4 v_Penumbras;

	// Values used for finding closest points and clipping.
	out vec3 v_Edges;

	// World space position.
	// TODO should be made relative to light center for precision?
	out vec3 v_WorldPosition;

	// Segment endpoints in world space divided by penetration depth.
	out vec4 v_SegmentData;
	
	layout(std140) uniform;
	uniform PhotonLocals {
		mat4 u_LightMatrix;
		mat4 u_MVP;
		float u_Radius;
	};
	
	vec2 transform(mat4 m, vec2 v){return (m*vec4(v, 0, 1)).xy;}
	vec3 transform(mat4 m, vec3 v){return (m*vec4(v.xy, 0, v.z)).xyw;}
	
	void main(){
		// Unpack input.
		float penetration = PhotonAttributePosition[2];
		v_Opacity = PhotonAttributePosition[3];
		
		vec2 segmentA = PhotonAttributeUV1;
		vec2 segmentB = PhotonAttributeUV2;
	
		// Determinant of the light matrix to check if it's flipped at all.
		float flip = sign(u_LightMatrix[0][0]*u_LightMatrix[1][1] - u_LightMatrix[0][1]*u_LightMatrix[1][0]);
		
		// Vertex projection.
		vec2 lightOffsetA = flip*vec2(-u_Radius,  u_Radius)*normalize(segmentA).yx;
		vec2 lightOffsetB = flip*vec2( u_Radius, -u_Radius)*normalize(segmentB).yx;
		
		vec2 occluderCoord = PhotonAttributePosition.xy;
		vec2 segmentPosition = mix(segmentA, segmentB, occluderCoord.x);
		vec2 projectionOffset = mix(lightOffsetA, lightOffsetB, occluderCoord.x);
		vec3 projected = vec3(segmentPosition - projectionOffset*occluderCoord.y, 1 - occluderCoord.y);
		vec3 clipPosition = transform(u_MVP, projected);
		gl_Position = vec4(clipPosition.xy, 0, clipPosition.z);
		
		// Penumbras.
		vec2 penumbraA = inverse(mat2(lightOffsetA, segmentA))*(projected.xy - segmentA*projected.z);
		vec2 penumbraB = inverse(mat2(lightOffsetB, segmentB))*(projected.xy - segmentB*projected.z);
		v_Penumbras = (u_Radius > 0 ? vec4(penumbraA, penumbraB) : vec4(0, 0, 1, 1));
		
		// Clipping/penetration values.
		vec2 segmentDelta = segmentB - segmentA;
		vec2 segmentSum = segmentA + segmentB;
		vec2 segmentNormal = segmentDelta.yx*vec2(-1, 1);
		
		// Handle the case where the light center is behind the axis.
		if(dot(segmentSum, segmentNormal) > 0){
			segmentDelta = -segmentDelta;
			segmentSum = reflect(segmentSum, segmentNormal);
		}
		
		v_Edges.xy = inverse(mat2(segmentDelta, segmentSum))*projected.xy;
		v_Edges.y *= 2.0;
		v_Edges.z = flip*dot(segmentNormal, projected.xy - segmentPosition*projected.z);
		
		// World space values.
		v_WorldPosition = vec3(transform(u_LightMatrix, projected).xy/penetration, clipPosition.z);
		vec2 segA = transform(u_LightMatrix, segmentA);
		vec2 segB = transform(u_LightMatrix, segmentB);
		v_SegmentData = vec4(segA, segB)/penetration;
	}
);

static const char *ShadowMaskFShader = PHOTON_GLSL(
	in float v_Opacity;
	in vec4 v_Penumbras;
	in vec3 v_Edges;
	in vec3 v_WorldPosition;
	in vec4 v_SegmentData;
	
	out vec4 PhotonFragOut;
	
	// Overcompensate penumbra edge by a few bits to ensure FP error cancels out.
	const float overshadow = 1.0 + 1.0/64.0;
	
	void main(){
		if(v_Edges.z >= 0.0) discard;
		
		// Light penetration.
		float closestT = clamp(v_Edges.x/abs(v_Edges.y), -0.5, 0.5) + 0.5;
		vec2 closestP = mix(v_SegmentData.xy, v_SegmentData.zw, closestT);
		float dist = min(length(closestP - v_WorldPosition.xy/v_WorldPosition.z), 1.0);
		float attenuation = dist*(3*dist - 2*dist*dist);
		
		// Penumbra mixing.
		vec2 p = clamp(v_Penumbras.xz/v_Penumbras.yw, -1, 1);
		vec2 value = mix(p*(3 - p*p)*0.25 + 0.5, vec2(1), step(v_Penumbras.yw, vec2(0)));
		float occlusion = (value[0] + value[1] - 1);
		
		PhotonFragOut = vec4(overshadow*v_Opacity*attenuation*occlusion);
	}
);

void
ChipmunkDebugDrawInit(void)
{
	Renderer = PhotonRendererNew();
	
	PhotonShader *directShader = PhotonShaderNew(DirectVShader, DirectFShader);
	PhotonUniforms *directUniforms = PhotonUniformsNew(directShader);
	
	PhotonShader *primitiveShader = PhotonShaderNew(PrimitiveVShader, PrimitiveFShader);
	PhotonUniforms *primitiveUniforms = PhotonUniformsNew(primitiveShader);
	PrimitiveState = PhotonRenderStateNew(&PhotonBlendModePremultipliedAlpha, primitiveUniforms);
	
	PhotonTextureOptions fontAtlasOptions = PhotonTextureOptionsDefault;
	fontAtlasOptions.format = PhotonTextureFormatR8;
	PhotonTexture *fontAtlas = PhotonTextureNew(sdf_tex_width, sdf_tex_height, sdf_data, &fontAtlasOptions);
	
	PhotonShader *fontShader = PhotonShaderNew(FontVShader, FontFShader);
	PhotonUniforms *fontUniforms = PhotonUniformsNew(fontShader);
	PhotonUniformsSetTexture(fontUniforms, "u_FontAtlas", fontAtlas);
	
	FontState = PhotonRenderStateNew(&PhotonBlendModePremultipliedAlpha, fontUniforms);
	
	// Fill in the glyph index array.
	for(int i=0; i<sdf_num_chars; i++){
		int char_index = sdf_spacing[i*8];
		glyph_indexes[char_index] = i;
	}
	
	ShadowMaskShader = PhotonShaderNew(ShadowMaskVShader, ShadowMaskFShader);
	
	static const PhotonBlendMode ShadowApplyBlend = {
		.colorOp = PhotonBlendOpAdd,
		.colorSrcFactor = PhotonBlendFactorDstAlpha,
		.colorDstFactor = PhotonBlendFactorOneMinusDstAlpha,
		.alphaOp = PhotonBlendOpAdd,
		.alphaSrcFactor = PhotonBlendFactorZero,
		.alphaDstFactor = PhotonBlendFactorZero,
	};
	
	ShadowApplyState = PhotonRenderStateNew(&ShadowApplyBlend, directUniforms);
}

static inline pvec4 MakeColor(cpSpaceDebugColor c){return (pvec4){{c.r, c.g, c.b, c.a}};}

static void
DrawCircle(pvec2 p, float r1, float r2, pvec4 color)
{
	pvec2 attribs = {r1, cpfmax(r2, 1)};
	
	PhotonRenderBuffers buffers = PhotonRendererEnqueueTriangles(Renderer, 2, 4, PrimitiveState);
	PhotonVertexPush(buffers.vertexes + 0, (pvec4){{p.x - r2, p.y - r2, 0, 1}}, (pvec2){-r2, -r2}, attribs, color);
	PhotonVertexPush(buffers.vertexes + 1, (pvec4){{p.x - r2, p.y + r2, 0, 1}}, (pvec2){-r2,  r2}, attribs, color);
	PhotonVertexPush(buffers.vertexes + 2, (pvec4){{p.x + r2, p.y + r2, 0, 1}}, (pvec2){ r2,  r2}, attribs, color);
	PhotonVertexPush(buffers.vertexes + 3, (pvec4){{p.x + r2, p.y - r2, 0, 1}}, (pvec2){ r2, -r2}, attribs, color);
	PhotonRenderBuffersCopyIndexes(&buffers, (PhotonIndex[]){0, 1, 2, 2, 3, 0}, 0, 6);
}

void
ChipmunkDebugDrawDot(cpFloat size, cpVect pos, cpSpaceDebugColor fill)
{
	float r = size*0.5f;
	DrawCircle((pvec2){pos.x, pos.y}, r + 1, r, MakeColor(fill));
}

void
ChipmunkDebugDrawCircle(cpVect pos, cpFloat angle, cpFloat radius, cpSpaceDebugColor fill)
{
	cpFloat r = radius + 1.0f/ChipmunkDebugDrawScaleFactor;
	DrawCircle((pvec2){pos.x, pos.y}, r - 1, r, MakeColor(fill));
	ChipmunkDebugDrawSegment(pos, cpvadd(pos, cpvmult(cpvforangle(angle), radius - ChipmunkDebugDrawScaleFactor*0.5f)), ChipmunkDebugDrawOutlineColor);
}

static void
DrawSegment(cpVect a, cpVect b, float r1, float r2, pvec4 color)
{
	cpVect t = cpvmult(cpvnormalize(cpvsub(b, a)), r2);
	pvec2 attribs = {r1, cpfmax(r2, 1)};
	
	PhotonRenderBuffers buffers = PhotonRendererEnqueueTriangles(Renderer, 6, 8, PrimitiveState);
	PhotonVertexPush(buffers.vertexes + 0, (pvec4){{b.x - t.y + t.x, b.y + t.x + t.y, 0, 1}}, (pvec2){ r2, -r2}, attribs, color);
	PhotonVertexPush(buffers.vertexes + 1, (pvec4){{b.x + t.y + t.x, b.y - t.x + t.y, 0, 1}}, (pvec2){ r2,  r2}, attribs, color);
	PhotonVertexPush(buffers.vertexes + 2, (pvec4){{b.x - t.y      , b.y + t.x      , 0, 1}}, (pvec2){  0, -r2}, attribs, color);
	PhotonVertexPush(buffers.vertexes + 3, (pvec4){{b.x + t.y      , b.y - t.x      , 0, 1}}, (pvec2){  0,  r2}, attribs, color);
	PhotonVertexPush(buffers.vertexes + 4, (pvec4){{a.x - t.y      , a.y + t.x      , 0, 1}}, (pvec2){  0, -r2}, attribs, color);
	PhotonVertexPush(buffers.vertexes + 5, (pvec4){{a.x + t.y      , a.y - t.x      , 0, 1}}, (pvec2){  0,  r2}, attribs, color);
	PhotonVertexPush(buffers.vertexes + 6, (pvec4){{a.x - t.y - t.x, a.y + t.x - t.y, 0, 1}}, (pvec2){-r2, -r2}, attribs, color);
	PhotonVertexPush(buffers.vertexes + 7, (pvec4){{a.x + t.y - t.x, a.y - t.x - t.y, 0, 1}}, (pvec2){-r2,  r2}, attribs, color);
	PhotonRenderBuffersCopyIndexes(&buffers, (PhotonIndex[]){0, 1, 2, 3, 1, 2, 3, 4, 2, 3, 4, 5, 6, 4, 5, 6, 7, 5}, 0, 18);
}

void
ChipmunkDebugDrawSegment(cpVect a, cpVect b, cpSpaceDebugColor color)
{
	DrawSegment(a, b, 2, 1, MakeColor(color));
}

void
ChipmunkDebugDrawFatSegment(cpVect a, cpVect b, cpFloat radius, cpSpaceDebugColor fill)
{
	float r = fmaxf(radius + 1/ChipmunkDebugDrawScaleFactor, 1);
	DrawSegment(a, b, r - 1, r, MakeColor(fill));
}

extern cpVect ChipmunkDemoMouse;

void
ChipmunkDebugDrawPolygon(int count, const cpVect *verts, cpFloat radius, cpSpaceDebugColor fill)
{
	pvec2 attribs = {1, 1};
	pvec4 color = MakeColor(fill);
	
	PhotonRenderBuffers buffers = PhotonRendererEnqueueTriangles(Renderer, count - 2, count, PrimitiveState);
	
	for(int i = 0; i < count; i++){
		buffers.vertexes = PhotonVertexPush(buffers.vertexes, (pvec4){{verts[i].x, verts[i].y, 0, 1}}, PVEC2_0, attribs, color);
	}
	
	for(int i = 0; i < count - 2; i++){
		buffers.indexes = PhotonIndexesCopy(buffers.indexes, (PhotonIndex[]){0, i + 1, i + 2}, 0, 3, buffers.batchOffset);
	}
}

/*
	struct ExtrudeVerts {cpVect offset, n;};
	size_t bytes = sizeof(struct ExtrudeVerts)*count;
	struct ExtrudeVerts *extrude = (struct ExtrudeVerts *)alloca(bytes);
	memset(extrude, 0, bytes);
	
	for(int i=0; i<count; i++){
		cpVect v0 = verts[(i-1+count)%count];
		cpVect v1 = verts[i];
		cpVect v2 = verts[(i+1)%count];
		
		cpVect n1 = cpvnormalize(cpvrperp(cpvsub(v1, v0)));
		cpVect n2 = cpvnormalize(cpvrperp(cpvsub(v2, v1)));
		
		cpVect offset = cpvmult(cpvadd(n1, n2), 1.0/(cpvdot(n1, n2) + 1.0f));
		struct ExtrudeVerts v = {offset, n2}; extrude[i] = v;
	}
	
//	Triangle *triangles = PushTriangles(6*count);
	Triangle *triangles = PushTriangles(5*count - 2);
	Triangle *cursor = triangles;
	
	cpFloat inset = -cpfmax(0.0f, 1.0f/ChipmunkDebugDrawPointLineScale - radius);
	for(int i=0; i<count-2; i++){
		struct v2f v0 = v2f(cpvadd(verts[  0], cpvmult(extrude[  0].offset, inset)));
		struct v2f v1 = v2f(cpvadd(verts[i+1], cpvmult(extrude[i+1].offset, inset)));
		struct v2f v2 = v2f(cpvadd(verts[i+2], cpvmult(extrude[i+2].offset, inset)));
		
		Triangle t = {{v0, v2f0, fillColor, fillColor}, {v1, v2f0, fillColor, fillColor}, {v2, v2f0, fillColor, fillColor}}; *cursor++ = t;
	}
	
	cpFloat outset = 1.0f/ChipmunkDebugDrawPointLineScale + radius - inset;
	for(int i=0, j=count-1; i<count; j=i, i++){
		cpVect vA = verts[i];
		cpVect vB = verts[j];
		
		cpVect nA = extrude[i].n;
		cpVect nB = extrude[j].n;
		
		cpVect offsetA = extrude[i].offset;
		cpVect offsetB = extrude[j].offset;
		
		cpVect innerA = cpvadd(vA, cpvmult(offsetA, inset));
		cpVect innerB = cpvadd(vB, cpvmult(offsetB, inset));
		
		// Admittedly my variable naming sucks here...
		struct v2f inner0 = v2f(innerA);
		struct v2f inner1 = v2f(innerB);
		struct v2f outer0 = v2f(cpvadd(innerA, cpvmult(nB, outset)));
		struct v2f outer1 = v2f(cpvadd(innerB, cpvmult(nB, outset)));
		struct v2f outer2 = v2f(cpvadd(innerA, cpvmult(offsetA, outset)));
		struct v2f outer3 = v2f(cpvadd(innerA, cpvmult(nA, outset)));
		
		struct v2f n0 = v2f(nA);
		struct v2f n1 = v2f(nB);
		struct v2f offset0 = v2f(offsetA);
		
		Triangle t0 = {{inner0, v2f0, fillColor, outlineColor}, {inner1,    v2f0, fillColor, outlineColor}, {outer1,      n1, fillColor, outlineColor}}; *cursor++ = t0;
		Triangle t1 = {{inner0, v2f0, fillColor, outlineColor}, {outer0,      n1, fillColor, outlineColor}, {outer1,      n1, fillColor, outlineColor}}; *cursor++ = t1;
		Triangle t2 = {{inner0, v2f0, fillColor, outlineColor}, {outer0,      n1, fillColor, outlineColor}, {outer2, offset0, fillColor, outlineColor}}; *cursor++ = t2;
		Triangle t3 = {{inner0, v2f0, fillColor, outlineColor}, {outer2, offset0, fillColor, outlineColor}, {outer3,      n0, fillColor, outlineColor}}; *cursor++ = t3;
	}
*/

void
ChipmunkDebugDrawBB(cpBB bb, cpSpaceDebugColor color)
{
	cpVect verts[] = {
		cpv(bb.r, bb.b),
		cpv(bb.r, bb.t),
		cpv(bb.l, bb.t),
		cpv(bb.l, bb.b),
	};
	ChipmunkDebugDrawPolygon(4, verts, 0.0f, color);
}

static float
PushChar(int character, float x, float y, pvec4 color)
{
	int i = glyph_indexes[character];
	float w = sdf_tex_width;
	float h = sdf_tex_height;
	
	float gw = sdf_spacing[i*8 + 3];
	float gh = sdf_spacing[i*8 + 4];
	
	float txmin = sdf_spacing[i*8 + 1]/w;
	float tymin = sdf_spacing[i*8 + 2]/h;
	float txmax = txmin + gw/w;
	float tymax = tymin + gh/h;
	
	float s = TextScale/scale_factor;
	float xmin = x + sdf_spacing[i*8 + 5]/scale_factor*TextScale;
	float ymin = y + (sdf_spacing[i*8 + 6]/scale_factor - gh)*TextScale;
	float xmax = xmin + gw*TextScale;
	float ymax = ymin + gh*TextScale;
	
	PhotonRenderBuffers buffers = PhotonRendererEnqueueTriangles(Renderer, 2, 4, FontState);
	PhotonVertexPush(buffers.vertexes + 0, (pvec4){{xmin, ymin, 0, 1}}, (pvec2){txmin, tymax}, PVEC2_0, color);
	PhotonVertexPush(buffers.vertexes + 1, (pvec4){{xmin, ymax, 0, 1}}, (pvec2){txmin, tymin}, PVEC2_0, color);
	PhotonVertexPush(buffers.vertexes + 2, (pvec4){{xmax, ymax, 0, 1}}, (pvec2){txmax, tymin}, PVEC2_0, color);
	PhotonVertexPush(buffers.vertexes + 3, (pvec4){{xmax, ymin, 0, 1}}, (pvec2){txmax, tymax}, PVEC2_0, color);
	PhotonRenderBuffersCopyIndexes(&buffers, (PhotonIndex[]){0, 1, 2, 0, 2, 3}, 0, 6);
	
	return sdf_spacing[i*8 + 7]*s;
	return 0;
}

void
ChipmunkDebugDrawText(cpVect pos, char const *str, cpSpaceDebugColor color)
{
	float x = pos.x, y = pos.y;
	pvec4 c = MakeColor(color);
	
	for(size_t i=0, len=strlen(str); i<len; i++){
		if(str[i] == '\n'){
			y -= TextLineHeight;
			x = pos.x;
		} else {
			x += PushChar(str[i], x, y, c);
		}
	}
}

cpTransform LightMatrixInv = {1, 0, 0, 1, 0, 0};

static void
ShadowsBegin(cpTransform mvp)
{
	static const PhotonBlendMode ShadowMaskBlend = {
		.colorOp = PhotonBlendOpAdd,
		.colorSrcFactor = PhotonBlendFactorZero,
		.colorDstFactor = PhotonBlendFactorOne,
		.alphaOp = PhotonBlendOpAdd,
		.alphaSrcFactor = PhotonBlendFactorOne,
		.alphaDstFactor = PhotonBlendFactorOne,
	};
	
	cpTransform l = cpTransformTranslate(ChipmunkDebugDrawLightPosition);
	cpTransform lmvp = cpTransformMult(mvp, l);
	
	struct {
		float u_lightMatrix[16];
		float u_MVP[16];
		float u_Radius;
	} shadowMaskLocals = {
		{
			l.a , l.b , 0, 0,
			l.c , l.d , 0, 0,
			0   ,   0 , 1, 0,
			l.tx, l.ty, 0, 1,
		},
		{
			lmvp.a , lmvp.b , 0, 0,
			lmvp.c , lmvp.d , 0, 0,
			0      ,      0 , 1, 0,
			lmvp.tx, lmvp.ty, 0, 1,
		},
		ChipmunkDebugDrawLightRadius,
	};
	
	PhotonUniforms *shadowMaskUniforms = PhotonRendererTemporaryUniforms(Renderer, ShadowMaskShader);
	PhotonUniformsSetLocals(shadowMaskUniforms, &shadowMaskLocals);
	
	ShadowMaskState = PhotonRendererTemporaryRenderState(Renderer, &ShadowMaskBlend, shadowMaskUniforms);
	
	LightMatrixInv = cpTransformInverse(l);
}

void
ChipmunkDebugDrawShadow(cpTransform transform, int count, cpVect *verts)
{
	const float penetration = 2;
	const float opacity = 1;
	
	PhotonRenderBuffers buffers = PhotonRendererEnqueueTriangles(Renderer, 2*count, 4*count, ShadowMaskState);
	
	cpTransform t = cpTransformMult(LightMatrixInv, transform);
	cpVect a = cpTransformPoint(t, verts[count - 1]);
	for(int i = 0; i < count; i++){
		cpVect b = cpTransformPoint(t, verts[i]);
		
		buffers.vertexes = PhotonVertexPush(buffers.vertexes, (pvec4){{0, 0, penetration, opacity}}, (pvec2){a.x, a.y}, (pvec2){b.x, b.y}, PVEC4_CLEAR);
		buffers.vertexes = PhotonVertexPush(buffers.vertexes, (pvec4){{0, 1, penetration, opacity}}, (pvec2){a.x, a.y}, (pvec2){b.x, b.y}, PVEC4_CLEAR);
		buffers.vertexes = PhotonVertexPush(buffers.vertexes, (pvec4){{1, 1, penetration, opacity}}, (pvec2){a.x, a.y}, (pvec2){b.x, b.y}, PVEC4_CLEAR);
		buffers.vertexes = PhotonVertexPush(buffers.vertexes, (pvec4){{1, 0, penetration, opacity}}, (pvec2){a.x, a.y}, (pvec2){b.x, b.y}, PVEC4_CLEAR);
		buffers.indexes = PhotonIndexesCopy(buffers.indexes, (PhotonIndex[]){0, 1, 2, 2, 3, 0}, 0, 6, buffers.batchOffset);
		
		a = b;
		buffers.batchOffset += 4;
	}
}

void
ChipmunkDebugDrawApplyShadows(void)
{
	const pvec4 c = {{0x75p-8, 0x4Fp-8, 0x44p-8, 1}};
	
	PhotonRenderBuffers buffers = PhotonRendererEnqueueTriangles(Renderer, 2, 4, ShadowApplyState);
	PhotonVertexPush(buffers.vertexes + 0, (pvec4){{-1, -1, 0, 1}}, PVEC2_0, PVEC2_0, c);
	PhotonVertexPush(buffers.vertexes + 1, (pvec4){{ 1, -1, 0, 1}}, PVEC2_0, PVEC2_0, c);
	PhotonVertexPush(buffers.vertexes + 2, (pvec4){{ 1,  1, 0, 1}}, PVEC2_0, PVEC2_0, c);
	PhotonVertexPush(buffers.vertexes + 3, (pvec4){{-1,  1, 0, 1}}, PVEC2_0, PVEC2_0, c);
	PhotonRenderBuffersCopyIndexes(&buffers, (PhotonIndex[]){0, 1, 2, 2, 3, 0}, 0, 6);
}

void
ChipmunkDebugDrawBegin(int width, int height)
{
	// TODO Need to make a set of renderers.
	while(!PhotonRendererWait(Renderer, 1)){
		printf("Sync on renderer\n");
	}
	
	PhotonRendererPrepare(Renderer, (pvec2){width, height});
	
	const pvec4 clear = {{0xECp-8, 0x73p-8, 0x57p-8, 0}};
	PhotonRendererBindRenderTexture(Renderer, NULL, PhotonLoadActionClear, PhotonStoreActionDontCare, clear);
	
	cpTransform p = ChipmunkDebugDrawProjection;
	cpTransform mvp = cpTransformMult(ChipmunkDebugDrawProjection, cpTransformInverse(ChipmunkDebugDrawCamera));
	
	struct {
		float u_P[16];
		float u_MVP[16];
		pvec4 u_OutlineColor;
		float u_OutlineWidth;
	} globals = {
		{
			p.a , p.b , 0, 0,
			p.c , p.d , 0, 0,
			0   ,   0 , 1, 0,
			p.tx, p.ty, 0, 1,
		},
		{
			mvp.a , mvp.b , 0, 0,
			mvp.c , mvp.d , 0, 0,
			    0 ,     0 , 1, 0,
			mvp.tx, mvp.ty, 0, 1,
		},
		MakeColor(ChipmunkDebugDrawOutlineColor),
		ChipmunkDebugDrawScaleFactor,
	};
	
	PhotonRendererSetGlobals(Renderer, &globals, sizeof(globals));
	
	ShadowsBegin(mvp);
}

void
ChipmunkDebugDrawFlush(void)
{
	PhotonRendererFlush(Renderer);
}
