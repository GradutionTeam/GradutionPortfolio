
Texture2D gBaseMap : register(t0);
Texture2D gEdgeMap : register(t1);

SamplerState gsamPointWrap        : register(s0);
SamplerState gsamPointClamp       : register(s1);
SamplerState gsamLinearWrap       : register(s2);
SamplerState gsamLinearClamp      : register(s3);
SamplerState gsamAnisotropicWrap  : register(s4);
SamplerState gsamAnisotropicClamp : register(s5);

static const float2 gTexCoords[6] = 
{
	float2(0, 1.0f ),  //왼쪽아래
	float2(0, 0),  //왼쪽위
	float2(1.0f , 0),  //오른쪽위
	float2(0, 1.0f),  //왼쪽 아래
	float2(1.0f , 0),  //오른쪽위
	float2(1.0f , 1.0f )   //오른쪽아래
};

struct VertexOut
{
	float4 PosH    : SV_POSITION;
	float2 TexC    : TEXCOORD;
};

VertexOut VS(uint vid : SV_VertexID)
{
	VertexOut vout;
	
	vout.TexC = gTexCoords[vid];
	
	// Map [0,1]^2 to NDC space.
	vout.PosH = float4(2.0f*vout.TexC.x - 1.0f, 1.0f - 2.0f*vout.TexC.y, 0.0f, 1.0f);

    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    float4 c = gBaseMap.SampleLevel(gsamPointClamp, pin.TexC, 0.0f);
	float4 e = gEdgeMap.SampleLevel(gsamPointClamp, pin.TexC, 0.0f);
	
	return c; // c * e
}


