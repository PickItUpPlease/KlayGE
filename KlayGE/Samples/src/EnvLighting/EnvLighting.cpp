#include <KlayGE/KlayGE.hpp>
#include <KFL/ThrowErr.hpp>
#include <KFL/Util.hpp>
#include <KFL/Math.hpp>
#include <KlayGE/Font.hpp>
#include <KlayGE/Renderable.hpp>
#include <KlayGE/RenderEngine.hpp>
#include <KlayGE/RenderEffect.hpp>
#include <KlayGE/FrameBuffer.hpp>
#include <KlayGE/SceneManager.hpp>
#include <KlayGE/Context.hpp>
#include <KlayGE/ResLoader.hpp>
#include <KlayGE/Texture.hpp>
#include <KlayGE/RenderableHelper.hpp>
#include <KlayGE/RenderSettings.hpp>
#include <KlayGE/Mesh.hpp>
#include <KlayGE/SceneObjectHelper.hpp>
#include <KlayGE/Camera.hpp>
#include <KlayGE/UI.hpp>
#include <KlayGE/PostProcess.hpp>

#include <KlayGE/RenderFactory.hpp>
#include <KlayGE/InputFactory.hpp>

#include <vector>
#include <sstream>

#include "SampleCommon.hpp"
#include "EnvLighting.hpp"

using namespace std;
using namespace KlayGE;

namespace
{
	class SphereRenderable : public StaticMesh
	{
	public:
		SphereRenderable(RenderModelPtr const & model, std::wstring const & /*name*/)
			: StaticMesh(model, L"Sphere")
		{
			RenderEffectPtr effect = SyncLoadRenderEffect("EnvLighting.fxml");
			techs_[0] = effect->TechniqueByName("Prefiltered");
			techs_[1] = effect->TechniqueByName("Approximate");
			techs_[2] = effect->TechniqueByName("GroundTruth");
			this->RenderingType(0);
		}

		void BuildMeshInfo()
		{
			AABBox const & pos_bb = this->PosBound();
			*(technique_->Effect().ParameterByName("pos_center")) = pos_bb.Center();
			*(technique_->Effect().ParameterByName("pos_extent")) = pos_bb.HalfSize();

			AABBox const & tc_bb = this->TexcoordBound();
			*(technique_->Effect().ParameterByName("tc_center")) = float2(tc_bb.Center().x(), tc_bb.Center().y());
			*(technique_->Effect().ParameterByName("tc_extent")) = float2(tc_bb.HalfSize().x(), tc_bb.HalfSize().y());
		}

		void CompressedCubeMap(TexturePtr const & y_cube, TexturePtr const & c_cube)
		{
			*(technique_->Effect().ParameterByName("skybox_Ycube_tex")) = y_cube;
			*(technique_->Effect().ParameterByName("skybox_Ccube_tex")) = c_cube;
		}

		void Roughness(float roughness)
		{
			*(technique_->Effect().ParameterByName("roughness")) = roughness;
		}

		void RenderingType(int type)
		{
			technique_ = techs_[type];
		}

		void IntegrateBRDFTex(TexturePtr const & tex)
		{
			*(technique_->Effect().ParameterByName("integrated_brdf_tex")) = tex;
		}

		void OnRenderBegin()
		{
			App3DFramework const & app = Context::Instance().AppInstance();
			Camera const & camera = app.ActiveCamera();

			float4x4 const mv = model_mat_ * camera.ViewMatrix();
			float4x4 const mvp = model_mat_ * camera.ViewProjMatrix();

			*(technique_->Effect().ParameterByName("model")) = model_mat_;
			*(technique_->Effect().ParameterByName("mvp")) = mvp;
			*(technique_->Effect().ParameterByName("eye_pos")) = camera.EyePos();
		}

	private:
		array<RenderTechniquePtr, 3> techs_;
	};

	class SphereObject : public SceneObjectHelper
	{
	public:
		SphereObject(TexturePtr const & y_cube, TexturePtr const & c_cube, float roughness)
			: SceneObjectHelper(SOA_Cullable)
		{
			renderable_ = SyncLoadModel("sphere_high.7z//sphere_high.meshml", EAH_GPU_Read | EAH_Immutable, CreateModelFactory<RenderModel>(), CreateMeshFactory<SphereRenderable>())->Mesh(0);
			checked_pointer_cast<SphereRenderable>(renderable_)->CompressedCubeMap(y_cube, c_cube);
			checked_pointer_cast<SphereRenderable>(renderable_)->Roughness(roughness);
		}

		void RenderingType(int type)
		{
			checked_pointer_cast<SphereRenderable>(renderable_)->RenderingType(type);
		}

		void IntegrateBRDFTex(TexturePtr const & tex)
		{
			checked_pointer_cast<SphereRenderable>(renderable_)->IntegrateBRDFTex(tex);
		}
	};

	enum
	{
		Exit,
	};

	InputActionDefine actions[] =
	{
		InputActionDefine(Exit, KS_Escape),
	};


	uint32_t ReverseBits(uint32_t bits)
	{
		bits = (bits << 16) | (bits >> 16);
		bits = ((bits & 0x55555555) << 1) | ((bits & 0xAAAAAAAA) >> 1);
		bits = ((bits & 0x33333333) << 2) | ((bits & 0xCCCCCCCC) >> 2);
		bits = ((bits & 0x0F0F0F0F) << 4) | ((bits & 0xF0F0F0F0) >> 4);
		bits = ((bits & 0x00FF00FF) << 8) | ((bits & 0xFF00FF00) >> 8);
		return bits;
	}

	float RadicalInverseVdC(uint32_t bits)
	{
		return ReverseBits(bits) * 2.3283064365386963e-10f; // / 0x100000000
	}

	float2 Hammersley2D(uint32_t i, uint32_t N)
	{
		return float2(static_cast<float>(i) / N, RadicalInverseVdC(i));
	}

	float3 ImportanceSampleGGX(float2 const & xi, float roughness)
	{
		float alpha = roughness * roughness;
		float phi = 2 * PI * xi.x();
		float cos_theta = sqrt((1 - xi.y()) / ((alpha * alpha - 1) * xi.y() + 1));
		float sin_theta = sqrt(1 - cos_theta * cos_theta);
		return float3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);
	}

	// http://graphicrants.blogspot.com.au/2013/08/specular-brdf-reference.html
	float G1(float n_dot_x, float roughness)
	{
		float alpha = roughness * roughness;
		float k = alpha * alpha;
		return 2 * n_dot_x / (n_dot_x + sqrt(k + (1 - k) * n_dot_x * n_dot_x));
	}

	float GSmith(float n_dot_v, float n_dot_l, float roughness)
	{
		return G1(n_dot_v, roughness) * G1(n_dot_l, roughness);
	}

	float2 IntegrateBRDFGGX(float roughness, float n_dot_v)
	{
		float3 view(sqrt(1.0f - n_dot_v * n_dot_v), 0, n_dot_v);
		float2 rg(0, 0);

		uint32_t const NUM_SAMPLES = 1024;
		for (uint32_t i = 0; i < NUM_SAMPLES; ++ i)
		{
			float2 xi = Hammersley2D(i, NUM_SAMPLES);
			float3 h = ImportanceSampleGGX(xi, roughness);
			float3 l = -MathLib::reflect(view, h);
			float n_dot_l = MathLib::clamp(l.z(), 0.0f, 1.0f);
			float n_dot_h = MathLib::clamp(h.z(), 0.0f, 1.0f);
			float v_dot_h = MathLib::clamp(MathLib::dot(view, h), 0.0f, 1.0f);
			if ((n_dot_l > 0) && (n_dot_h * n_dot_v != 0))
			{
				float g = GSmith(n_dot_v, n_dot_l, roughness);
				float g_vis = g * v_dot_h / std::max(1e-6f, n_dot_h * n_dot_v);
				float fc = pow(1 - v_dot_h, 5);
				rg += float2(1 - fc, fc) * g_vis;
			}
		}

		return rg / NUM_SAMPLES;
	}

	TexturePtr GenIntegrateBRDF()
	{
		uint32_t const WIDTH = 128;
		uint32_t const HEIGHT = 128;

		std::vector<uint8_t> integrate_brdf(WIDTH * HEIGHT * 2);
		for (uint32_t y = 0; y < HEIGHT; ++ y)
		{
			float roughness = (y + 0.5f) / HEIGHT;
			for (uint32_t x = 0; x < WIDTH; ++ x)
			{
				float cos_theta = (x + 0.5f) / WIDTH;

				float2 lut = IntegrateBRDFGGX(roughness, cos_theta);
				integrate_brdf[(y * WIDTH + x) * 2 + 0] = static_cast<uint8_t>(MathLib::clamp(static_cast<int>(lut.x() * 255 + 0.5f), 0, 255));
				integrate_brdf[(y * WIDTH + x) * 2 + 1] = static_cast<uint8_t>(MathLib::clamp(static_cast<int>(lut.y() * 255 + 0.5f), 0, 255));
			}
		}

		std::vector<ElementInitData> init_data(1);
		init_data[0].data = &integrate_brdf[0];
		init_data[0].row_pitch = WIDTH * 2;
		init_data[0].slice_pitch = HEIGHT * init_data[0].row_pitch;
		
		RenderFactory& rf = Context::Instance().RenderFactoryInstance();
		return rf.MakeTexture2D(WIDTH, HEIGHT, 1, 1, EF_GR8, 1, 0, EAH_GPU_Read | EAH_Immutable, &init_data[0]);
	}
}


int SampleMain()
{
	EnvLightingApp app;
	app.Create();
	app.Run();

	return 0;
}

EnvLightingApp::EnvLightingApp()
		: App3DFramework("EnvLighting"),
			obj_controller_(true, MB_Left, MB_Middle, 0)
{
	ResLoader::Instance().AddPath("../../Samples/media/EnvLighting");
}

bool EnvLightingApp::ConfirmDevice() const
{
	RenderDeviceCaps const & caps = Context::Instance().RenderFactoryInstance().RenderEngineInstance().DeviceCaps();
	if (caps.max_shader_model < 4)
	{
		return false;
	}

	return true;
}

void EnvLightingApp::InitObjects()
{
	font_ = SyncLoadFont("gkai00mp.kfont");

	y_cube_map_ = SyncLoadTexture("uffizi_cross_filtered_y.dds", EAH_GPU_Read | EAH_Immutable);
	c_cube_map_ = SyncLoadTexture("uffizi_cross_filtered_c.dds", EAH_GPU_Read | EAH_Immutable);
	integrate_brdf_tex_ = GenIntegrateBRDF();

	spheres_.resize(10);
	for (size_t i = 0; i < spheres_.size(); ++ i)
	{
		spheres_[i] = MakeSharedPtr<SphereObject>(y_cube_map_, c_cube_map_, 1 - static_cast<float>(i) / (spheres_.size() - 1));
		spheres_[i]->ModelMatrix(MathLib::scaling(1.3f, 1.3f, 1.3f)
			* MathLib::translation((-static_cast<float>(spheres_.size() / 2) + i + 0.5f) * 0.08f, 0.0f, 0.0f));
		checked_pointer_cast<SphereObject>(spheres_[i])->IntegrateBRDFTex(integrate_brdf_tex_);
		spheres_[i]->AddToSceneManager();
	}

	sky_box_ = MakeSharedPtr<SceneObjectSkyBox>(0);
	checked_pointer_cast<SceneObjectSkyBox>(sky_box_)->CompressedCubeMap(y_cube_map_, c_cube_map_);
	sky_box_->AddToSceneManager();

	this->LookAt(float3(0.0f, 0.2f, -0.6f), float3(0, 0, 0));
	this->Proj(0.05f, 100);

	obj_controller_.AttachCamera(this->ActiveCamera());
	obj_controller_.Scalers(0.003f, 0.003f);

	InputEngine& inputEngine(Context::Instance().InputFactoryInstance().InputEngineInstance());
	InputActionMap actionMap;
	actionMap.AddActions(actions, actions + sizeof(actions) / sizeof(actions[0]));

	action_handler_t input_handler = MakeSharedPtr<input_signal>();
	input_handler->connect(KlayGE::bind(&EnvLightingApp::InputHandler, this, KlayGE::placeholders::_1, KlayGE::placeholders::_2));
	inputEngine.ActionMap(actionMap, input_handler);

	UIManager::Instance().Load(ResLoader::Instance().Open("EnvLighting.uiml"));

	dialog_ = UIManager::Instance().GetDialog("Method");
	id_type_combo_ = dialog_->IDFromName("TypeCombo");

	dialog_->Control<UIComboBox>(id_type_combo_)->OnSelectionChangedEvent().connect(KlayGE::bind(&EnvLightingApp::TypeChangedHandler, this, KlayGE::placeholders::_1));
	this->TypeChangedHandler(*dialog_->Control<UIComboBox>(id_type_combo_));
}

void EnvLightingApp::OnResize(uint32_t width, uint32_t height)
{
	App3DFramework::OnResize(width, height);

	UIManager::Instance().SettleCtrls();
}

void EnvLightingApp::InputHandler(InputEngine const & /*sender*/, InputAction const & action)
{
	switch (action.first)
	{
	case Exit:
		this->Quit();
		break;
	}
}

void EnvLightingApp::TypeChangedHandler(KlayGE::UIComboBox const & sender)
{
	rendering_type_ = sender.GetSelectedIndex();
	for (size_t i = 0; i < spheres_.size(); ++ i)
	{
		checked_pointer_cast<SphereObject>(spheres_[i])->RenderingType(rendering_type_);
	}
}

void EnvLightingApp::DoUpdateOverlay()
{
	UIManager::Instance().Render();

	std::wostringstream stream;
	stream.precision(2);
	stream << std::fixed << this->FPS() << " FPS";

	font_->RenderText(0, 0, Color(1, 1, 0, 1), L"Env Lighting", 16);
	font_->RenderText(0, 18, Color(1, 1, 0, 1), stream.str(), 16);
}

uint32_t EnvLightingApp::DoUpdate(uint32_t /*pass*/)
{
	RenderEngine& re = Context::Instance().RenderFactoryInstance().RenderEngineInstance();
	re.CurFrameBuffer()->Attached(FrameBuffer::ATT_DepthStencil)->ClearDepthStencil(1.0f, 0);

	return App3DFramework::URV_NeedFlush | App3DFramework::URV_Finished;
}