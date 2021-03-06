#include "Scene01.h"

#include <iostream>


MyScene::MyScene(HINSTANCE hInstance)
	: D3DApp(hInstance)
{
	mSceneBounds.Center = XMFLOAT3(0.0f, 0.0f, 0.0f);
	mSceneBounds.Radius = sqrtf(1500.0f*1500.0f + 3000.0f*3000.0f);
}

MyScene::~MyScene()
{
}

bool MyScene::Initialize()
{
	NetWork::getInstance()->setPlayerState(CS_NONE);
	if (!D3DApp::Initialize())
		return false;

	// Reset the command list to prep for initialization commands.
	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

	mCbvSrvUavDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	//md3dDevice->

#ifdef _DEBUG
#pragma comment(linker,"/entry:WinMainCRTStartup /subsystem:console")
#endif

	mBlurFilter = std::make_unique<BlurFilter>(md3dDevice.Get(),
		mClientWidth, mClientHeight, DXGI_FORMAT_R8G8B8A8_UNORM);

	mSobelFilter = std::make_unique<SobelFilter>(
		md3dDevice.Get(),
		mClientWidth, mClientHeight,
		mBackBufferFormat);

	mOffscreenRT = std::make_unique<RenderTarget>(
		md3dDevice.Get(),
		mClientWidth, mClientHeight,
		mBackBufferFormat);

	mCamera.SetPosition(0.0f, 0.0f, -800.0f);

	mShadowMap = std::make_unique<ShadowMap>(
		md3dDevice.Get(), 4096, 4096);

	LoadTextures();
	BuildRootSignature();
	BuildPostProcessRootSignature();
	BuildPostProcessSobelRootSignature();
	BuildDescriptorHeaps();
	BuildShadersAndInputLayout();
	BuildShapeGeometry();
	BuildFlareSpritesGeometry();
	BuildFbxGeometry("Model/HandLight.fbx", "handLightGeo", "handLight", 1.0f, false, false);
	BuildFbxGeometry("Model/SplitMetalBall.fbx", "leverGeo", "lever", 1.0f, false, false);
	BuildFbxGeometry("Model/Statue.fbx", "statueGeo", "statue", 1.0f, false, false);
	BuildFbxGeometry("Model/doorFrame.obj", "doorFrameGeo", "doorFrame", 1.0f, false, false);
	BuildFbxGeometry("Model/door.obj", "doorGeo", "door", 1.0f, false, false);
	BuildFbxGeometry("Model/moveingTile.obj", "tileGeo", "tile00", 1, true, false);
	BuildFbxGeometry("Model/Spear.fbx", "spearGeo", "spear", 1, false, false);//장애물(창)
	BuildFbxGeometry("Model/robotFree3.fbx", "robot_freeGeo", "robot_free", 1.0f, false, true);//angle  robotModel  robotIdle
	//BuildFbxGeometry("Model/TestModel.fbx", "robot_freeGeo", "robot_free", 1.0f, false, true);
	//튜토리얼 맵
	BuildFbxGeometry("Model/tutorial.obj", "map00Geo", "map00", 1, true, false);

	//스테이지 2
	BuildFbxGeometry("Model/Map02.obj", "map02Geo", "map02", 1, true, false);
	BuildFbxGeometry("Model/Map02_lope.obj", "map02_00Geo", "map02_00", 1, true, false); //다리

	BuildAnimation("Model/robotFree3.fbx", "walk", 1.0f, false);//robotwalk

		//BuildAnimation("Model/TestIdle.fbx", "walk", 1.0f, false);
	BuildMaterials();
	BuildGameObjects();
	BuildFrameResources();

	BuildPSOs();

	// Execute the initialization commands.
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	// Wait until initialization is complete.
	FlushCommandQueue();

	return true;
}

void MyScene::CreateRtvAndDsvDescriptorHeaps()
{
	// Add +1 descriptor for offscreen render target.
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
	rtvHeapDesc.NumDescriptors = SwapChainBufferCount + 1;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvHeapDesc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
		&rtvHeapDesc, IID_PPV_ARGS(mRtvHeap.GetAddressOf())));

	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
	dsvHeapDesc.NumDescriptors = 3;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
		&dsvHeapDesc, IID_PPV_ARGS(mDsvHeap.GetAddressOf())));
}

void MyScene::OnResize()
{
	if (mSobelFilter != nullptr)
	{
		mSobelFilter->OnResize(mClientWidth, mClientHeight);
	}
	if (mBlurFilter != nullptr)
	{
		mBlurFilter->OnResize(mClientWidth, mClientHeight);
	}
	if (mOffscreenRT != nullptr)
	{
		mOffscreenRT->OnResize(mClientWidth, mClientHeight);
	}
	mCamera.SetLens(0.25f*MathHelper::Pi, AspectRatio(), 1.0f, 10000.0f);

	mCamera.UpdateViewMatrix();

	BoundingFrustum::CreateFromMatrix(mCamFrustum, mCamera.GetProj());

	D3DApp::OnResize();
}

void MyScene::Update(const GameTimer& gt)
{
	OnKeyboardInput(gt);

	// 순환적으로 자원 프레임 배열의 다음 원소에 접근한다
	mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
	mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

	// Has the GPU finished processing the commands of the current frame resource?
	// If not, wait until the GPU has completed commands up to this fence point.
	// GPU가 현재 프레임 자원의 명령들을 다 처리했는지 확인한다. 아직
	// 다 처리하지 않았으면 GPU가 이 울타리 지점까지의 명령들을 처리할 때까지 기다린다
	if (mCurrFrameResource->Fence != 0 && mFence->GetCompletedValue() < mCurrFrameResource->Fence)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
		ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, eventHandle));
		WaitForSingleObject(eventHandle, INFINITE);
		CloseHandle(eventHandle);
	}

	if (nowScene == (int)Scene::Menu)
		MenuSceneUpdate(gt);
	else if (nowScene != (int)Scene::Menu) {
		GameSceneUpdate(gt);
		mEnergy -= gt.DeltaTime()*0.01f;

		//게임오버시
		if (mEnergy <= 0) {
			NetWork::getInstance()->SendMsg(CS_DIE, NetWork::getInstance()->GetPlayerPosition(), NetWork::getInstance()->getWorldPos(ePlayer));
			mSkinnedModelInst->SetNowAni("die");
			blurLevel = 2;
		}
	}
}

void MyScene::Draw(const GameTimer& gt)
{
	auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;
	// 명령 기록에 관련된 메모리의 재활용을 위해 명령 할당자를 재설정한다.
	// 재설정은 GPU가 관련 명령 목ㅇ록들을 모두 처리한 후에 일어남
	ThrowIfFailed(cmdListAlloc->Reset());

	// 명령 목록을 ExecuteCommandList를 통해서 명령 대기열에
	// 추가했다면 명령 목록을 재설정할 수 있다. 명령 목록을
	// 재설정하면 메모리가 재활용된다
	ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mPSOs["opaque"].Get()));

	//게임상태일 때 렌더링
	if (nowScene == (int)Scene::Menu)
		MenuSceneRender(gt);
	if (nowScene != (int)Scene::Menu)
		GameSceneRender(gt);

	// Done recording commands.
	ThrowIfFailed(mCommandList->Close());
	// 명령 실행을 위해 명령 목록을 명령 대기열에 추가한다.
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);
	// 후면 버퍼와 전면 버퍼 교환
	ThrowIfFailed(mSwapChain->Present(0, 0));
	mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;
	// 현재 울타리 지점까지의 명령들을 표시하도록 울타리 값을 전진시킨다.
	mCurrFrameResource->Fence = ++mCurrentFence;

	// 새 울타리 지점을 설정하는 명령을 명령 대기열에 추가한다.
	// 지금 우리는 GPU 시간선 상에 있으므로, 새 울타리 지점은 GPU가
	// 이 시크날 명령 이전까지의 모든 명령을 처리하기 전까지는 설정되지 않는다
	mCommandQueue->Signal(mFence.Get(), mCurrentFence);
}

void MyScene::MenuSceneKeyboardInput(const GameTimer& gt)
{
	if (GetAsyncKeyState('1') & 0x8000) {
		nowScene = (int)Scene::Scene01;
		InitGameScene();
	}
	if (GetAsyncKeyState('2') & 0x8000) {
		nowScene = (int)Scene::Scene02;
		InitGameScene();
	}

	auto player = mOpaqueRitems[(int)RenderLayer::Player];
	NetWork::getInstance()->SendMsg(CS_POS, player[0]->GetPosition(), player[0]->World);

}
void MyScene::MenuSceneUpdate(const GameTimer& gt)
{
	UpdateObjectCBs(gt);
	UpdateMaterialBuffer(gt);
	UpdateMainPassCB(gt);
}
void MyScene::MenuSceneRender(const GameTimer& gt)
{
	mCommandList->RSSetViewports(1, &mScreenViewport);
	mCommandList->RSSetScissorRects(1, &mScissorRect);

	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

	mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::DarkBlue, 0, nullptr);
	mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

	mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

	ID3D12DescriptorHeap* descriptorHeaps[] = { mCbvSrvUavDescriptorHeap.Get() };
	mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

	mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

	auto passCB = mCurrFrameResource->PassCB->Resource();
	mCommandList->SetGraphicsRootConstantBufferView(2, passCB->GetGPUVirtualAddress());


	mCommandList->SetPipelineState(mPSOs["ui"].Get());
	DrawGameObjects(mCommandList.Get(), mOpaqueRitems[(int)RenderLayer::Menu], (int)RenderLayer::Menu);
	mCommandList->SetPipelineState(mPSOs["uiAp"].Get());
	DrawGameObjects(mCommandList.Get(), mOpaqueRitems[(int)RenderLayer::MenuButton], (int)RenderLayer::MenuButton);
	// Indicate a state transition on the resource usage.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));
}

void MyScene::InitGameScene()
{
	auto player = mOpaqueRitems[(int)RenderLayer::Player];
	auto items = mOpaqueRitems[(int)RenderLayer::Item];
	auto lever = mOpaqueRitems[(int)RenderLayer::Lever];
	auto spear = mOpaqueRitems[(int)RenderLayer::Spear];
	auto flare = mOpaqueRitems[(int)RenderLayer::Flare];

	auto movetile = mOpaqueRitems[(int)RenderLayer::MoveTile];

	//플레이어 초기화
	//뭐뭐 초기화
	if (nowScene == (int)Scene::Scene01) {
		blurLevel = 0;
		mEnergy = 0.5f;
		//카메라 초기화
		mCamera.SetPosition(player[0]->GetPosition().x, player[0]->GetPosition().y + 2000, player[0]->GetPosition().z - 500);
		mCamera.LookAt(mCamera.GetPosition3f(), player[0]->GetPosition(), XMFLOAT3(0, 1, 0));

		player[0]->SetPosition(XMFLOAT3(-200, 300.0f, -1300));

		movetile[0]->SetPosition(XMFLOAT3(0.0f, -250.0f, 300.0f));
		movetile[1]->SetPosition(XMFLOAT3(-10000.0f, -2000.0f, 0));
		movetile[2]->SetPosition(XMFLOAT3(-10000.0f, -2000.0f, 0));

		items[0]->SetPosition(XMFLOAT3(-1500, items[0]->GetPosition().y, -580));
		lever[0]->SetPosition(XMFLOAT3(1500, lever[0]->GetPosition().y, -600));
		lever[1]->SetPosition(XMFLOAT3(-300, lever[1]->GetPosition().y, 1500));
		lever[2]->SetPosition(XMFLOAT3(-10000, lever[2]->GetPosition().y, 0));

		for(int i = 0; i<flare.size(); ++i)
			flare[i]->SetPosition(XMFLOAT3(-10000, 3000, 0));
		for (int i = 0; i<spear.size(); ++i)
			spear[i]->SetPosition(XMFLOAT3(-10000, 3000, 0));

		player[0]->isOnGround = false;

		NetWork::getInstance()->setPlayerState(CS_NONE);

		NetWork::getInstance()->SendItemState(CS_ITEM_SET, 0, movetile[0]->GetPosition());
		NetWork::getInstance()->SendItemState(CS_ITEM_SET, 1, movetile[1]->GetPosition());
		NetWork::getInstance()->SendItemState(CS_ITEM_SET, 2, movetile[2]->GetPosition());

		NetWork::getInstance()->SendMsg(CS_POS, XMFLOAT3(-200, 300.0f, -1300), player[0]->World);
		
		mSkinnedModelInst->SetNowAni("idle");

	}
	else if (nowScene == (int)Scene::Scene02) {
		blurLevel = 0;
		mEnergy = 0.5f;
		//카메라 초기화
		mCamera.SetPosition(player[0]->GetPosition().x, player[0]->GetPosition().y + 2000, player[0]->GetPosition().z - 500);
		mCamera.LookAt(mCamera.GetPosition3f(), player[0]->GetPosition(), XMFLOAT3(0, 1, 0));

		player[0]->SetPosition(XMFLOAT3(-1800, 300.0f, 1500)); //시작
		//player[0]->SetPosition(XMFLOAT3(300, 150, -1150)); //밑
		//player[0]->SetPosition(XMFLOAT3(-300, 300.0f, 1500)); // 옆

		movetile[0]->SetPosition(XMFLOAT3(-2100.0f, -250.0f, -300.0f));
		movetile[1]->SetPosition(XMFLOAT3(2100.0f, -250.0f, 300));
		movetile[2]->SetPosition(XMFLOAT3(2100.0f, -250.0f, -600));

		items[0]->SetPosition(XMFLOAT3(-2100, items[0]->GetPosition().y, 1800));

		for (int i = 0; i<flare.size(); ++i)
			flare[i]->SetPosition(XMFLOAT3(-10000, 3000, 0));

		int index = 0;
		for (int i = 0; i < 4; ++i) {
			for (int j = 0; j < 4; ++j) {
				if ( (i + j) % 2 == 0) {
					spear[index]->SetPosition(XMFLOAT3(-900 + 300 * i, 3000, -300 - 300 * j));
					++index;
				}
				else {
					continue;
				}
			}
		}

		lever[0]->SetPosition(XMFLOAT3(300, lever[0]->GetPosition().y, 900)); // 0 -> 레버 0
		lever[1]->SetPosition(XMFLOAT3(-2400, lever[1]->GetPosition().y, -900)); // 1 -> 레버 1
		lever[2]->SetPosition(XMFLOAT3(0, lever[2]->GetPosition().y, -300)); // 2 -> 레버 2작동
		player[0]->isOnGround = false;

		NetWork::getInstance()->setPlayerState(CS_NONE);
		//NetWork::getInstance()->SetItemPosition({ -2100.0f, -250.0f, -300.0f });

		NetWork::getInstance()->SetItemPosition({ movetile[0]->GetPosition() }, 0);
		NetWork::getInstance()->SetItemPosition({ movetile[1]->GetPosition() }, 1);
		NetWork::getInstance()->SetItemPosition({ movetile[2]->GetPosition() }, 2);

		NetWork::getInstance()->SendMsg(CS_POS, XMFLOAT3(-1800, 300.0f, 1500), player[0]->World);
		NetWork::getInstance()->SendItemState(CS_ITEM_SET, 0, movetile[0]->GetPosition());
		NetWork::getInstance()->SendItemState(CS_ITEM_SET, 1, movetile[1]->GetPosition());
		NetWork::getInstance()->SendItemState(CS_ITEM_SET, 2, movetile[2]->GetPosition());
		
		mSkinnedModelInst->SetNowAni("idle");

	}
	else if (nowScene == (int)Scene::Menu) {
		mCamera.SetPosition(0.0f, 0.0f, -800.0f);
		mCamera.SetLook(XMFLOAT3(0, 0, 1));
		mCamera.SetUp(XMFLOAT3(0, 1, 0));
	}
}

void MyScene::GameSceneUpdate(const GameTimer& gt)
{
	mLightRotationAngle += 0.001f*gt.DeltaTime();
	XMMATRIX R = XMMatrixRotationY(mLightRotationAngle);
	for (int i = 0; i < 3; ++i)
	{
		XMVECTOR lightDir = XMLoadFloat3(&mBaseLightDirections[i]);
		lightDir = XMVector3TransformNormal(lightDir, R);
		XMStoreFloat3(&mRotatedLightDirections[i], lightDir);
	}

	AnimateMaterials(gt);
	UpdateSkinnedCBs(gt);
	UpdateObjectCBs(gt);
	UpdateMaterialBuffer(gt);
	UpdateShadowTransform(gt);
	UpdateMainPassCB(gt);
	UpdateShadowPassCB(gt);
}


void MyScene::GameSceneRender(const GameTimer& gt)
{
	/////////////////////////////////// 그림자 선 렌더링 ///////////////////////////////////////
	ID3D12DescriptorHeap* descriptorHeaps[] = { mCbvSrvUavDescriptorHeap.Get() };
	mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

	mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

	auto matBuffer = mCurrFrameResource->MaterialCB->Resource();
	mCommandList->SetGraphicsRootConstantBufferView(3, matBuffer->GetGPUVirtualAddress());
	// Bind null SRV for shadow map pass.
	mCommandList->SetGraphicsRootDescriptorTable(7, mNullSrv);
	DrawSceneToShadowMap();
	mCommandList->SetGraphicsRootDescriptorTable(0, mCbvSrvUavDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

	///////////////////////////////////////////////////////////////////////////////////////////

	// 뷰포트와 가위 직사각형 설정
	// 명령 목록을 재설정할 때마다 재설정
	mCommandList->RSSetViewports(1, &mScreenViewport);
	mCommandList->RSSetScissorRects(1, &mScissorRect);
	// 자원 용도에 관련된 상태 전이를 Direct3D에 통지한다
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mOffscreenRT->Resource(),
		D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));
	// 후면 버퍼와 깊이 버퍼를 지운다.
	mCommandList->ClearRenderTargetView(mOffscreenRT->Rtv(), Colors::Black, 0, nullptr);
	mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
	// 렌더링 결과가 기록될 렌더 대상 버퍼들을 지정
	mCommandList->OMSetRenderTargets(1, &mOffscreenRT->Rtv(), true, &DepthStencilView());

	////////////////////////////////////////////////////////////

	auto passCB = mCurrFrameResource->PassCB->Resource();
	mCommandList->SetGraphicsRootConstantBufferView(2, passCB->GetGPUVirtualAddress());

	CD3DX12_GPU_DESCRIPTOR_HANDLE skyTexDescriptor(mCbvSrvUavDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	skyTexDescriptor.Offset(mSkyTexHeapIndex + 1, mCbvSrvUavDescriptorSize);
	mCommandList->SetGraphicsRootDescriptorTable(7, skyTexDescriptor);

	//mCommandList->SetPipelineState(mPSOs["debug"].Get());
	//DrawGameObjects(mCommandList.Get(), mOpaqueRitems[(int)RenderLayer::Debug], (int)RenderLayer::Debug);
	if (mIsWireframe)
	{
		mCommandList->SetPipelineState(mPSOs["opaque_wireframe"].Get());
	}
	else
	{
		mCommandList->SetPipelineState(mPSOs["grid"].Get());
	}
	DrawGameObjects(mCommandList.Get(), mOpaqueRitems[(int)RenderLayer::Lever], (int)RenderLayer::Lever);
	DrawGameObjects(mCommandList.Get(), mOpaqueRitems[(int)RenderLayer::Item], (int)RenderLayer::Item);
	if (nowScene == 1)
		DrawGameObjects(mCommandList.Get(), mOpaqueRitems[(int)RenderLayer::Scene01_Map], (int)RenderLayer::Scene01_Map);
	else if (nowScene == 2) {
		DrawGameObjects(mCommandList.Get(), mOpaqueRitems[(int)RenderLayer::Scene02_Map], (int)RenderLayer::Scene02_Map);
		DrawGameObjects(mCommandList.Get(), mOpaqueRitems[(int)RenderLayer::Spear], (int)RenderLayer::Spear);
	}

	DrawGameObjects(mCommandList.Get(), mOpaqueRitems[(int)RenderLayer::MoveTile], (int)RenderLayer::MoveTile);
	DrawGameObjects(mCommandList.Get(), mOpaqueRitems[(int)RenderLayer::Opaque], (int)RenderLayer::Opaque);
	//DrawGameObjects(mCommandList.Get(), mOpaqueRitems[(int)RenderLayer::CollBox], (int)RenderLayer::CollBox);
	//
	if (mIsWireframe)
		mCommandList->SetPipelineState(mPSOs["opaque_wireframe"].Get());
	else
		mCommandList->SetPipelineState(mPSOs["skinnedOpaque"].Get());

	DrawGameObjects(mCommandList.Get(), mOpaqueRitems[(int)RenderLayer::Player], (int)RenderLayer::Player);
	DrawGameObjects(mCommandList.Get(), mOpaqueRitems[(int)RenderLayer::Friend], (int)RenderLayer::Friend);

	mCommandList->SetPipelineState(mPSOs["flareSprites"].Get());
	DrawGameObjects(mCommandList.Get(), mOpaqueRitems[(int)RenderLayer::Flare], (int)RenderLayer::Flare);

	mCommandList->SetPipelineState(mPSOs["uiMove"].Get());
	DrawGameObjects(mCommandList.Get(), mOpaqueRitems[(int)RenderLayer::MoveUI], (int)RenderLayer::MoveUI);
	mCommandList->SetPipelineState(mPSOs["uiAp"].Get());
	DrawGameObjects(mCommandList.Get(), mOpaqueRitems[(int)RenderLayer::BaseUI], (int)RenderLayer::BaseUI);

	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mOffscreenRT->Resource(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));
	mSobelFilter->Execute(mCommandList.Get(), mPostProcessSobelRootSignature.Get(),
		mPSOs["sobel"].Get(), mOffscreenRT->Srv());
	mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

	mCommandList->SetGraphicsRootSignature(mPostProcessSobelRootSignature.Get());
	mCommandList->SetPipelineState(mPSOs["composite"].Get());
	mCommandList->SetGraphicsRootDescriptorTable(0, mOffscreenRT->Srv());
	mCommandList->SetGraphicsRootDescriptorTable(1, mSobelFilter->OutputSrv());
	DrawFullscreenQuad(mCommandList.Get());

	//blur 
	mBlurFilter->Execute(mCommandList.Get(), mPostProcessRootSignature.Get(),
		mPSOs["horzBlur"].Get(), mPSOs["vertBlur"].Get(), CurrentBackBuffer(), blurLevel);
	// Prepare to copy blurred output to the back buffer.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
	mCommandList->CopyResource(CurrentBackBuffer(), mBlurFilter->Output());
	// Indicate a state transition on the resource usage.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));
}

void MyScene::GameSceneKeyboardInput(const GameTimer& gt)
{
	const float dt = gt.DeltaTime();

	if (GetAsyncKeyState(VK_F2) & 0x8000) {
		OnResize();
		return;
	}
	if (GetAsyncKeyState(VK_MENU) & 0x8000) {
		OnResize();
		return;
	}

	if (GetAsyncKeyState('1') & 0x8000) mIsWireframe = true;
	else mIsWireframe = false;

	if (GetAsyncKeyState('2') & 0x8000) blurLevel = 0;
	if (GetAsyncKeyState('3') & 0x8000) blurLevel = 1;
	if (GetAsyncKeyState('4') & 0x8000) blurLevel = 2;
	if (GetAsyncKeyState('5') & 0x8000) {
		mEnergy -= 0.001f;
		//	printf("%.2f\n", mEnergy);
	}
	if (GetAsyncKeyState('6') & 0x8000) {
		mEnergy += 0.001f;

		auto& player = mOpaqueRitems[(int)RenderLayer::Player];
		//printf("%.2f  %.2f  %.2f\n", player[0]->GetPosition().x, player[0]->GetPosition().y, player[0]->GetPosition().z);
	}
	if (GetAsyncKeyState('7') & 0x8000)
	{
		mTimer.Stop();
	}
	if (GetAsyncKeyState('8') & 0x8000)
	{
		mTimer.Start();
	}
	if (GetAsyncKeyState('Q') & 0x8000) {
		auto& player = mOpaqueRitems[(int)RenderLayer::Player];
		player[0]->SetPosition(XMFLOAT3(-1800, 300.0f, 0));
	}


	if (GetAsyncKeyState(VK_SPACE) & 0x8000) {
	}

	if (GetAsyncKeyState('W') & 0x8000) {
		mCamera.Walk(1000.0f *dt);
	}
	if (GetAsyncKeyState('S') & 0x8000) {
		mCamera.Walk(-1000.0f *dt);
	}
	if (GetAsyncKeyState('A') & 0x8000) {
		mCamera.Strafe(-1000.0f *dt);
	}
	if (GetAsyncKeyState('D') & 0x8000) {
		mCamera.Strafe(1000.0f *dt);
	}

	if (mEnergy <= 0) {
		if (GetAsyncKeyState(VK_RETURN) & 0x8000) {
			nowScene = (int)Scene::Menu;
			InitGameScene();
		}
	}

	//게임오버 전에
	auto& eplayer = mOpaqueRitems[(int)RenderLayer::Player];
	if (mEnergy > 0)
		if (GetAsyncKeyState(VK_LEFT) || GetAsyncKeyState(VK_RIGHT) || GetAsyncKeyState(VK_UP) || GetAsyncKeyState(VK_DOWN)) {

			mSkinnedModelInst->SetNowAni("walk");
			if (eplayer[0]->isOnGround == true)
			{
				if ((GetAsyncKeyState(VK_UP) && GetAsyncKeyState(VK_RIGHT))) {
					if (NetWork::getInstance()->getPlayerState() != CS_RIGHT_UP) {
						NetWork::getInstance()->setPlayerState(CS_RIGHT_UP);
						//NetWork::getInstance()->SendMsg(CS_RIGHT_UP, eplayer[0]->GetPosition(), eplayer[0]->World);
						NetWork::getInstance()->SendKeyDown(CS_RIGHT_UP);
					}
				}
				else if ((GetAsyncKeyState(VK_DOWN) && GetAsyncKeyState(VK_RIGHT))) {
					if (NetWork::getInstance()->getPlayerState() != CS_RIGHT_DOWN) {
						NetWork::getInstance()->setPlayerState(CS_RIGHT_DOWN);
						//NetWork::getInstance()->SendMsg(CS_RIGHT_DOWN, eplayer[0]->GetPosition(), eplayer[0]->World);
						NetWork::getInstance()->SendKeyDown(CS_RIGHT_DOWN);
					}
				}
				else if ((GetAsyncKeyState(VK_UP) && GetAsyncKeyState(VK_LEFT))) {
					if (NetWork::getInstance()->getPlayerState() != CS_LEFT_UP) {
						NetWork::getInstance()->setPlayerState(CS_LEFT_UP);
						//NetWork::getInstance()->SendMsg(CS_LEFT_UP, eplayer[0]->GetPosition(), eplayer[0]->World);
						NetWork::getInstance()->SendKeyDown(CS_LEFT_UP);

					}
				}
				else if ((GetAsyncKeyState(VK_DOWN) && GetAsyncKeyState(VK_LEFT))) {
					if (NetWork::getInstance()->getPlayerState() != CS_LEFT_DOWN) {
						NetWork::getInstance()->setPlayerState(CS_LEFT_DOWN);
						//NetWork::getInstance()->SendMsg(CS_LEFT_DOWN, eplayer[0]->GetPosition(), eplayer[0]->World);
						NetWork::getInstance()->SendKeyDown(CS_LEFT_DOWN);
					}
				}
				else if (GetAsyncKeyState(VK_UP) & 0x8000) {
					if (NetWork::getInstance()->getPlayerState() != CS_UP) {
						NetWork::getInstance()->setPlayerState(CS_UP);
						//NetWork::getInstance()->SendMsg(CS_UP, eplayer[0]->GetPosition(), eplayer[0]->World);
						NetWork::getInstance()->SendKeyDown(CS_UP);
					}
				}
				else if (GetAsyncKeyState(VK_DOWN) & 0x8000) {
					if (NetWork::getInstance()->getPlayerState() != CS_DOWN) {
						NetWork::getInstance()->setPlayerState(CS_DOWN);
						//	NetWork::getInstance()->SendMsg(CS_DOWN, eplayer[0]->GetPosition(), eplayer[0]->World);
						NetWork::getInstance()->SendKeyDown(CS_DOWN);
					}
				}
				else if (GetAsyncKeyState(VK_LEFT) & 0x8000) {
					if (NetWork::getInstance()->getPlayerState() != CS_LEFT) {
						NetWork::getInstance()->setPlayerState(CS_LEFT);
						//NetWork::getInstance()->SendMsg(CS_LEFT, eplayer[0]->GetPosition(), eplayer[0]->World);
						NetWork::getInstance()->SendKeyDown(CS_LEFT);
					}
				}
				else if (GetAsyncKeyState(VK_RIGHT) & 0x8000) {
					if (NetWork::getInstance()->getPlayerState() != CS_RIGHT) {
						NetWork::getInstance()->setPlayerState(CS_RIGHT);
						//NetWork::getInstance()->SendMsg(CS_RIGHT, eplayer[0]->GetPosition(), eplayer[0]->World);
						NetWork::getInstance()->SendKeyDown(CS_RIGHT);
					}
				}
			}
		}
		else {
			mSkinnedModelInst->SetNowAni("idle");
			if (NetWork::getInstance()->getPlayerState() != CS_NONE) {
				NetWork::getInstance()->setPlayerState(CS_NONE);
				//NetWork::getInstance()->SendMsg(CS_POS, eplayer[0]->GetPosition(), eplayer[0]->World);
				NetWork::getInstance()->SendKeyDown(CS_NONE);
			}
		}

		//printf("%.2f %.2f %.2f\n", eplayer[0]->GetPosition().x, eplayer[0]->GetPosition().y, eplayer[0]->GetPosition().z);
		mCamera.UpdateViewMatrix();
}

void MyScene::OnMouseDown(WPARAM btnState, int x, int y)
{
	if (nowScene == (int)Scene::Menu)
	{
		if ((btnState & MK_LBUTTON) != 0)
		{
			if (startMouseOnVal) {
				nowScene = (int)Scene::Scene01;
				InitGameScene();
			}
			else if (quitMouseOnVal) exit(0);
		}
	}
	SetCapture(mhMainWnd);
}

void MyScene::OnMouseUp(WPARAM btnState, int x, int y)
{
	ReleaseCapture();
}

void MyScene::OnMouseMove(WPARAM btnState, int x, int y)
{
	if (nowScene == (int)Scene::Menu)
	{
		int buttonSize = mClientWidth / 3;
		int buttonX = mClientWidth / 3 + mClientWidth / 2;
		int startbuttonY = mClientHeight / 5 + mClientHeight / 2;
		int quitbuttonY = mClientHeight / 3 + mClientHeight / 2 + mClientHeight / 30;
		bool a = x > buttonX - buttonSize / 3;
		bool b = x < buttonX + buttonSize / 3;
		bool c = y > startbuttonY - buttonSize / 8;
		bool d = y < startbuttonY + buttonSize / 8;
		bool e = y > quitbuttonY - buttonSize / 8;
		bool f = y < quitbuttonY + buttonSize / 8;
		{
			if (a && b && c && d)startMouseOnVal = true;
			else startMouseOnVal = false;
			if (a && b && e && f) quitMouseOnVal = true;
			else quitMouseOnVal = false;
		}
	}
	else if (nowScene != (int)Scene::Menu) {
		//if ((btnState & MK_LBUTTON) != 0)
		//{
		//	// Make each pixel correspond to a quarter of a degree.
		//	float dx = XMConvertToRadians(0.25f*static_cast<float>(x - mLastMousePos.x));
		//	float dy = XMConvertToRadians(0.25f*static_cast<float>(y - mLastMousePos.y));

		//	mCamera.Pitch(dy);
		//	mCamera.RotateY(dx);
		//}
		//if ((btnState & MK_RBUTTON) != 0)
		//{
		//	// Make each pixel correspond to a quarter of a degree.
		//	mx = XMConvertToRadians(0.25f*static_cast<float>(x - mLastMousePos.x));
		//	my = XMConvertToRadians(0.25f*static_cast<float>(y - mLastMousePos.y));

		//}
		//mLastMousePos.x = x;
		//mLastMousePos.y = y;
	}
}

void MyScene::OnKeyboardInput(const GameTimer& gt)
{
	if (nowScene == (int)Scene::Menu)
		MenuSceneKeyboardInput(gt);
	if (nowScene != (int)Scene::Menu)
		GameSceneKeyboardInput(gt);
}

void MyScene::AnimateMaterials(const GameTimer& gt)
{

}

void MyScene::UpdateSkinnedCBs(const GameTimer& gt)
{
	auto currSkinnedCB = mCurrFrameResource->SkinnedCB.get();

	mSkinnedModelInst->UpdateSkinnedAnimation(gt.DeltaTime());
	SkinnedConstants skinnedConstants;
	std::copy(
		std::begin(mSkinnedModelInst->FinalTransforms),
		std::end(mSkinnedModelInst->FinalTransforms),
		&skinnedConstants.BoneTransforms[0]);
	currSkinnedCB->CopyData(0, skinnedConstants);

	auto currFriendSkinnedCB = mCurrFrameResource->SkinnedCB.get();
	SkinnedConstants skinnedFriendConstants;
	mSkinnedFriendModelInst->UpdateSkinnedAnimation(gt.DeltaTime());
	std::copy(
		std::begin(mSkinnedFriendModelInst->FinalTransforms),
		std::end(mSkinnedFriendModelInst->FinalTransforms),
		&skinnedFriendConstants.BoneTransforms[0]);

	currFriendSkinnedCB->CopyData(1, skinnedFriendConstants);
}

void MyScene::UpdateObjectCBs(const GameTimer& gt)
{
	auto currObjectCB = mCurrFrameResource->ObjectCB.get();

	//mCamera.Pitch(my);
	//mCamera.RotateY(mx);

	if (nowScene == (int)Scene::Menu)
	{
		auto menuButtons = mOpaqueRitems[(int)RenderLayer::MenuButton];
		//버튼들 start 0,1  quit 2,3
		if (startMouseOnVal == false) {
			XMStoreFloat4x4(&menuButtons[0]->World, XMMatrixScaling(mClientWidth / 3, mClientWidth / 3, 1.0f) *XMMatrixTranslation(mClientWidth / 6, -mClientHeight / 8, 0.0f));
			XMStoreFloat4x4(&menuButtons[1]->World, XMMatrixScaling(0, 0, 0));
		}
		else {
			XMStoreFloat4x4(&menuButtons[1]->World, XMMatrixScaling(mClientWidth / 3, mClientWidth / 3, 1.0f)*XMMatrixTranslation(mClientWidth / 6, -mClientHeight / 8, 0.0f));
			XMStoreFloat4x4(&menuButtons[0]->World, XMMatrixScaling(0, 0, 0));
		}
		if (quitMouseOnVal == false) {
			XMStoreFloat4x4(&menuButtons[2]->World, XMMatrixScaling(mClientWidth / 3, mClientWidth / 3, 1.0f)*XMMatrixTranslation(mClientWidth / 6, -mClientHeight / 4, 0.0f));
			XMStoreFloat4x4(&menuButtons[3]->World, XMMatrixScaling(0, 0, 0));
		}
		else {
			XMStoreFloat4x4(&menuButtons[3]->World, XMMatrixScaling(mClientWidth / 3, mClientWidth / 3, 1.0f)*XMMatrixTranslation(mClientWidth / 6, -mClientHeight / 4, 0.0f));
			XMStoreFloat4x4(&menuButtons[2]->World, XMMatrixScaling(0, 0, 0));
		}
		for (auto& e : mOpaqueRitems[(int)RenderLayer::Menu]) {
			//메뉴창
			XMStoreFloat4x4(&e->World, XMMatrixScaling(mClientWidth, mClientHeight, 1.0f)*XMMatrixTranslation(-mClientWidth / 2, mClientHeight / 2, 0.0f));
			e->NumFramesDirty = gNumFrameResources;
		}
		for (auto& e : mOpaqueRitems[(int)RenderLayer::MenuButton]) {
			e->NumFramesDirty = gNumFrameResources;
		}
	}
	else if (nowScene != (int)Scene::Menu)
	{
		auto player = mOpaqueRitems[(int)RenderLayer::Player];
		mCamera.SetPosition(player[0]->GetPosition().x, player[0]->GetPosition().y + 2000, player[0]->GetPosition().z - 600);

		for (auto& e : mOpaqueRitems[(int)RenderLayer::Player])
		{
			auto item = mOpaqueRitems[(int)RenderLayer::Item];
			auto lever = mOpaqueRitems[(int)RenderLayer::Lever];
			auto mt = mOpaqueRitems[(int)RenderLayer::MoveTile];
			auto mfriend = mOpaqueRitems[(int)RenderLayer::Friend];
			//auto rand = mOpaqueRitems[(int)RenderLayer::Scene01_Map];
			auto cols = mOpaqueRitems[(int)RenderLayer::MapCollision01];
			auto spear = mOpaqueRitems[(int)RenderLayer::Spear];

			printf("%.2f %.2f %.2f\n", e->GetPosition().x, e->GetPosition().y, e->GetPosition().z);

			if (e->isOnGround == false) {
				e->GravityUpdate(gt);
				NetWork::getInstance()->SetWorldPotision(ePlayer, e->World);
				NetWork::getInstance()->setPlayerState(CS_NONE);
				NetWork::getInstance()->SendMsg(CS_POS, { 0,0,0 }, NetWork::getInstance()->getWorldPos(ePlayer));
			}
			e->World = (NetWork::getInstance()->getWorldPos(ePlayer));

			if (NetWork::getInstance()->getAniState(ePlayer) == 0)		mSkinnedModelInst->SetNowAni("idle");
			else if (NetWork::getInstance()->getAniState(ePlayer) == 1)	mSkinnedModelInst->SetNowAni("walk");
			else if (NetWork::getInstance()->getAniState(ePlayer) == 2)	mSkinnedModelInst->SetNowAni("die");

			//정적인 맵과의 충돌에서 움직일 때만 처리
			if (nowScene == 1) {
				cols = mOpaqueRitems[(int)RenderLayer::MapCollision01];
			}
			else if (nowScene == 2) {
				cols = mOpaqueRitems[(int)RenderLayer::MapCollision02];
			}
			{
				e->isOnGround = false;
				for (int i = 0; i < cols.size(); i++) {
					//온그라운드가 true일 경우 또 true인지 검사한다?
					if (e->bounds.IsCollsionAABB(e->GetPosition(), &cols[i]->bounds, cols[i]->GetPosition()))
					{
						//충돌체 검사가 통과되면 보정 후, 온 그라운드를 활성화 시킨다.
						//보정안함
						e->isOnGround = true;
						break;
					}
					else {
						mSkinnedModelInst->SetNowAni("idle");
						e->isOnGround = false;
					}
				}

				//그리드와의 충돌은 float끼리만 계산하면 된다 평면이니까
				if (e->isOnGround == false)
					if (e->GetPosition().y - e->bounds.GetMin().y < -250.0f) {
						e->isOnGround = true;
						mEnergy = 0.0f;
						//printf("떨어져 죽음 \n");
					}
			}

			if (e->bounds.IsCollsionAABB(e->GetPosition(), &item[0]->bounds, item[0]->GetPosition())) {
				//printf("아이템과 충돌 \n");
				mEnergy += gt.DeltaTime() * 0.9f;
				if (mEnergy >= 1.0f) mEnergy = 1.0f;
			}

			//장애물과 충돌
			for (int i = 0; i < spear.size(); i++) {
				if (e->bounds.IsCollsionAABB(e->GetPosition(), &spear[i]->bounds, spear[i]->GetPosition()))
				{
					mEnergy = 0;
					break;
				}
			}

			//if (e->bounds.IsCollsionAABB(e->GetPosition(), &mfriend[0]->bounds, mfriend[0]->GetPosition()))
				//printf("플레이어간 충돌 \n");


			//레버와 충돌시 스테이지 1
			{
				if (nowScene == (int)Scene::Scene01) {
					if (e->bounds.IsCollsionAABB(e->GetPosition(), &lever[0]->bounds, lever[0]->GetPosition()))
					{
						lever[0]->RotateY(gt.DeltaTime() * 25);
						isLeverOn = true; //임시변수임
						NetWork::getInstance()->setLever(isLeverOn, 0);
						if (NetWork::getInstance()->getItemState(0) != CS_ITEM_ON)
							NetWork::getInstance()->SendItemState(CS_ITEM_ON, 0, mt[0]->GetPosition());
					}
					else if (e->bounds.IsCollsionAABB(e->GetPosition(), &lever[1]->bounds, lever[1]->GetPosition()))
					{
						lever[1]->RotateY(gt.DeltaTime() * 25);
						isLeverOn = true; //임시변수임
						NetWork::getInstance()->setLever(isLeverOn, 0);
						if (NetWork::getInstance()->getItemState(0) != CS_ITEM_ON)
							NetWork::getInstance()->SendItemState(CS_ITEM_ON, 0, mt[0]->GetPosition());
					}
					//비활성화일 경우
					else
					{
						isLeverOn = false;
						NetWork::getInstance()->setLever(isLeverOn, 0);
						if (NetWork::getInstance()->getItemState(0) != CS_ITEM_OFF)
							NetWork::getInstance()->SendItemState(CS_ITEM_OFF, 0, mt[0]->GetPosition());

					}
				}
				else if (nowScene == (int)Scene::Scene02) {
					if (e->bounds.IsCollsionAABB(e->GetPosition(), &lever[0]->bounds, lever[0]->GetPosition()))
					{
						lever[0]->RotateY(gt.DeltaTime() * 25);
						isLeverOn = true; //임시변수임
						NetWork::getInstance()->setLever(isLeverOn, 0);
						if (NetWork::getInstance()->getItemState(0) != CS_ITEM_ON)
							NetWork::getInstance()->SendItemState(CS_ITEM_ON, 0, mt[0]->GetPosition());
					}
					else
					{
						isLeverOn = false;
						NetWork::getInstance()->setLever(isLeverOn, 0);
						if (NetWork::getInstance()->getItemState(0) != CS_ITEM_OFF)
							NetWork::getInstance()->SendItemState(CS_ITEM_OFF, 0, mt[0]->GetPosition());

					}
					/////////////////////////
					if (e->bounds.IsCollsionAABB(e->GetPosition(), &lever[1]->bounds, lever[1]->GetPosition()))
					{
						lever[1]->RotateY(gt.DeltaTime() * 25);
						isLeverOn = true; //임시변수임
						NetWork::getInstance()->setLever(isLeverOn, 1);
						if (NetWork::getInstance()->getItemState(1) != CS_ITEM_ON)
							NetWork::getInstance()->SendItemState(CS_ITEM_ON, 1, mt[1]->GetPosition());
					}
					else
					{
						isLeverOn = false;
						NetWork::getInstance()->setLever(isLeverOn, 1);
						if (NetWork::getInstance()->getItemState(1) != CS_ITEM_OFF)
							NetWork::getInstance()->SendItemState(CS_ITEM_OFF, 1, mt[1]->GetPosition());
					}
					if (e->bounds.IsCollsionAABB(e->GetPosition(), &lever[2]->bounds, lever[2]->GetPosition()))
					{
						lever[2]->RotateY(gt.DeltaTime() * 25);
						isLeverOn = true;
						NetWork::getInstance()->setLever(isLeverOn, 2);
						if (NetWork::getInstance()->getItemState(2) != CS_ITEM_ON)
							NetWork::getInstance()->SendItemState(CS_ITEM_ON, 2, mt[2]->GetPosition());
					}
					else
					{
						isLeverOn = false;
						NetWork::getInstance()->setLever(isLeverOn, 2);
						if (NetWork::getInstance()->getItemState(2) != CS_ITEM_OFF)
							NetWork::getInstance()->SendItemState(CS_ITEM_OFF, 2, mt[2]->GetPosition());
					}
				}
			}

			//플레이어 갱신
			e->NumFramesDirty = gNumFrameResources;
		}

		//동료
		for (auto& e : mOpaqueRitems[(int)RenderLayer::Friend])
		{
			//cout << NetWork::getInstance()->getAniState(eFriend) << endl;
			if (0 == NetWork::getInstance()->getAniState(eFriend))		 mSkinnedFriendModelInst->SetNowAni("idle");
			else if (1 == NetWork::getInstance()->getAniState(eFriend))  mSkinnedFriendModelInst->SetNowAni("walk");
			else if (2 == NetWork::getInstance()->getAniState(eFriend))  mSkinnedFriendModelInst->SetNowAni("die");

			if (CS_UP <= NetWork::getInstance()->getFriendState() && NetWork::getInstance()->getFriendState() <= CS_NONE) {
				e->World = (NetWork::getInstance()->getWorldPos(eFriend));
			}


			e->NumFramesDirty = gNumFrameResources;
		}

		//움직이는 발판 
		int tileIndex = 0;
		for (auto& e : mOpaqueRitems[(int)RenderLayer::MoveTile])
		{
			if (nowScene == (int)Scene::Scene01)
			{
				if (tileIndex >= 1) {
					++tileIndex;
					e->NumFramesDirty = gNumFrameResources;
					continue;
				}
			}

			auto& eplayer = mOpaqueRitems[(int)RenderLayer::Player];

			e->SetPosition(XMFLOAT3(NetWork::getInstance()->GetItemPosition(tileIndex)));

			//if (tileIndex == 1)
			//	printf("%d번 %d %.2f \n", tileIndex, isLeverOn, e->GetPosition().y);

			if (NetWork::getInstance()->GetItemPosition(tileIndex).y > -250)
				NetWork::getInstance()->setLever(true, tileIndex);

			isLeverOn = NetWork::getInstance()->getLever(tileIndex);
			
			

			//플레이어가 발판에 서있으면
			if (e->bounds.IsCollsionAABB(e->GetPosition(), &eplayer[0]->bounds, eplayer[0]->GetPosition()))
			{
				eplayer[0]->isOnGround = true;
				if (isLeverOn == false) {
					NetWork::getInstance()->SendMsg(CS_DIE, NetWork::getInstance()->GetPlayerPosition(), NetWork::getInstance()->getWorldPos(ePlayer));
					mEnergy = 0.0f; //플레이어 죽음
				}
				else if (isLeverOn == true) {
					//플레이어도 같이 움직임
					eplayer[0]->SetPosition(XMFLOAT3(eplayer[0]->GetPosition().x, eplayer[0]->GetPosition().y + 80 * gt.DeltaTime(), eplayer[0]->GetPosition().z));
				}
			}

			++ tileIndex;
			e->NumFramesDirty = gNumFrameResources;
		}
		//아이템
		for (auto& e : mOpaqueRitems[(int)RenderLayer::Item])
		{
			//회전
			e->RotateY(gt.DeltaTime() * 1);
			e->NumFramesDirty = gNumFrameResources;
		}
		//장애물
		for (auto& e : mOpaqueRitems[(int)RenderLayer::Spear])
		{
			if (nowScene == (int)Scene::Scene01) break;
			auto cols = mOpaqueRitems[(int)RenderLayer::MapCollision02];
			for (int i = 0; i < cols.size(); i++) {
				if (e->bounds.IsCollsionAABB(e->GetPosition(), &cols[i]->bounds, cols[i]->GetPosition()))
				{
					//이펙트 생성
					for (auto& fl : mOpaqueRitems[(int)RenderLayer::Flare])
					{
						fl->m_texAniStartTime = mMainPassCB.TotalTime;
						if (!fl->m_bActive) {
							fl->m_bActive = true;
							fl->SetPosition(e->GetPosition().x, e->GetPosition().y -250, e->GetPosition().z );
							break;
						}
					}
					
					if (e->GetPosition().x == -900)e->SetPosition(e->GetPosition().x + 300, e->GetPosition().y, e->GetPosition().z);
					else if (e->GetPosition().x == -600)e->SetPosition(e->GetPosition().x - 300, e->GetPosition().y, e->GetPosition().z);
					else if (e->GetPosition().x == -300)e->SetPosition(e->GetPosition().x + 300, e->GetPosition().y, e->GetPosition().z);
					else if (e->GetPosition().x == 0)e->SetPosition(e->GetPosition().x - 300, e->GetPosition().y, e->GetPosition().z);
					e->SetPosition(e->GetPosition().x, 3000, e->GetPosition().z);
				}
			}
			//회전
			e->RotateY(gt.DeltaTime() * 20);
			e->SetPosition(e->GetPosition().x, e->GetPosition().y - gt.DeltaTime() * 2000, e->GetPosition().z);
			e->NumFramesDirty = gNumFrameResources;
		}
		//이펙트
		for (auto& e : mOpaqueRitems[(int)RenderLayer::Flare])
		{
			if (e->m_bActive) {
				if (e->m_texAniStartTime + e->m_texAniTime < gt.TotalTime()) {
					e->m_texAniIndex++;
					if (e->m_texAniIndex >= 25) {//25개프레임
						e->m_texAniIndex = 0;
						e->m_bActive = false;
						e->SetPosition(-10000, 3000, 0);
					}
					e->m_texAniStartTime = gt.TotalTime();
				}
			}
			e->NumFramesDirty = gNumFrameResources;
		}
		//상수오브젝트
		for (auto& e : mOpaqueRitems[(int)RenderLayer::Opaque])
		{
			e->NumFramesDirty = gNumFrameResources;
		}
		for (auto& e : mOpaqueRitems[(int)RenderLayer::Lever])
		{
			e->NumFramesDirty = gNumFrameResources;
		}

		for (auto& e : mOpaqueRitems[(int)RenderLayer::CollBox])
		{
			auto player = mOpaqueRitems[(int)RenderLayer::Player];
			auto mfriend = mOpaqueRitems[(int)RenderLayer::Friend];
			auto item = mOpaqueRitems[(int)RenderLayer::Item];
			auto tile = mOpaqueRitems[(int)RenderLayer::MoveTile];

			if (e->Geo->Name == "robot_freeBoxGeo") {
				e->SetPosition(player[0]->GetPosition());
			}
			else if (e->Geo->Name == "dummy_freeBoxGeo") {
				e->SetPosition(mfriend[0]->GetPosition());
			}
			else if (e->Geo->Name == "handLightBoxGeo") {
				e->SetPosition(item[0]->GetPosition());
			}
			else if (e->Geo->Name == "tileBoxGeo") {
				e->SetPosition(tile[0]->GetPosition().x, tile[0]->GetPosition().y - 150, tile[0]->GetPosition().z);
			}

			e->NumFramesDirty = gNumFrameResources;
		}


		//UIs
		for (auto& e : mOpaqueRitems[(int)RenderLayer::BaseUI]) {

			e->SetLook3f(mCamera.GetLook3f());
			e->SetRight3f(mCamera.GetRight3f());
			e->SetUp3f(mCamera.GetUp3f());
			e->SetPosition(Vector3::Add(mCamera.GetPosition3f(), Vector3::ScalarProduct(mCamera.GetLook3f(), 5, false)));
			e->SetPosition(Vector3::Add(e->GetPosition(), Vector3::ScalarProduct(e->GetRight3f(), -3.0f, false)));
			e->SetPosition(Vector3::Add(e->GetPosition(), Vector3::ScalarProduct(e->GetUp3f(), -0.5, false)));
			e->NumFramesDirty = gNumFrameResources;
		}
		for (auto& e : mOpaqueRitems[(int)RenderLayer::MoveUI]) {
			e->SetLook3f(mCamera.GetLook3f());
			e->SetRight3f(mCamera.GetRight3f());
			e->SetUp3f(mCamera.GetUp3f());
			e->SetPosition(Vector3::Add(mCamera.GetPosition3f(), Vector3::ScalarProduct(mCamera.GetLook3f(), 5, false)));
			e->SetPosition(Vector3::Add(e->GetPosition(), Vector3::ScalarProduct(e->GetRight3f(), -3.0f, false)));
			e->SetPosition(Vector3::Add(e->GetPosition(), Vector3::ScalarProduct(e->GetUp3f(), -0.5, false)));
			e->NumFramesDirty = gNumFrameResources;
		}
	}

	for (auto& e : mAllRitems)
	{
		if (e->NumFramesDirty > 0)
		{
			XMMATRIX world = XMLoadFloat4x4(&e->World);
			XMMATRIX texTransform = XMLoadFloat4x4(&e->TexTransform);

			ObjectConstants objConstants;
			XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
			XMStoreFloat4x4(&objConstants.TexTransform, XMMatrixTranspose(texTransform));

			objConstants.TextureAniIndex = e->m_texAniIndex;
			currObjectCB->CopyData(e->ObjCBIndex, objConstants);
			e->NumFramesDirty--;
		}
	}
	mx = 0;
	my = 0;
	mCamera.UpdateViewMatrix();
}

void MyScene::UpdateMaterialBuffer(const GameTimer& gt)
{
	auto currMaterialBuffer = mCurrFrameResource->MaterialCB.get();
	for (auto& e : mMaterials)
	{
		// Only update the cbuffer data if the constants have changed.  If the cbuffer
		// data changes, it needs to be updated for each FrameResource.
		Material* mat = e.second.get();
		if (mat->NumFramesDirty > 0)
		{
			XMMATRIX matTransform = XMLoadFloat4x4(&mat->MatTransform);

			MaterialConstants matConstants;
			matConstants.DiffuseAlbedo = mat->DiffuseAlbedo;
			matConstants.FresnelR0 = mat->FresnelR0;
			matConstants.Roughness = mat->Roughness;
			XMStoreFloat4x4(&matConstants.MatTransform, XMMatrixTranspose(matTransform));
			matConstants.DiffuseMapIndex = mat->DiffuseSrvHeapIndex;

			currMaterialBuffer->CopyData(mat->MatCBIndex, matConstants);

			// Next FrameResource need to be updated too.
			mat->NumFramesDirty--;
		}
	}
}

void MyScene::UpdateShadowTransform(const GameTimer& gt)
{
	// Only the first "main" light casts a shadow.
	XMVECTOR lightDir = XMLoadFloat3(&mRotatedLightDirections[0]);
	XMVECTOR lightPos = -2.0f*mSceneBounds.Radius*lightDir;
	XMVECTOR targetPos = XMLoadFloat3(&mSceneBounds.Center);
	XMVECTOR lightUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
	XMMATRIX lightView = XMMatrixLookAtLH(lightPos, targetPos, lightUp);

	XMStoreFloat3(&mLightPosW, lightPos);

	// Transform bounding sphere to light space.
	XMFLOAT3 sphereCenterLS;
	XMStoreFloat3(&sphereCenterLS, XMVector3TransformCoord(targetPos, lightView));

	// Ortho frustum in light space encloses scene.
	float l = sphereCenterLS.x - mSceneBounds.Radius;
	float b = sphereCenterLS.y - mSceneBounds.Radius;
	float n = sphereCenterLS.z - mSceneBounds.Radius;
	float r = sphereCenterLS.x + mSceneBounds.Radius;
	float t = sphereCenterLS.y + mSceneBounds.Radius;
	float f = sphereCenterLS.z + mSceneBounds.Radius;

	mLightNearZ = n;
	mLightFarZ = f;
	XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(l, r, b, t, n, f);

	// Transform NDC space [-1,+1]^2 to texture space [0,1]^2
	XMMATRIX T(
		0.5f, 0.0f, 0.0f, 0.0f,
		0.0f, -0.5f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.5f, 0.5f, 0.0f, 1.0f);

	XMMATRIX S = lightView * lightProj*T;
	XMStoreFloat4x4(&mLightView, lightView);
	XMStoreFloat4x4(&mLightProj, lightProj);
	XMStoreFloat4x4(&mShadowTransform, S);
}

void MyScene::UpdateMainPassCB(const GameTimer& gt)
{
	XMMATRIX view = mCamera.GetView();
	XMMATRIX proj = mCamera.GetProj();

	XMMATRIX viewProj = XMMatrixMultiply(view, proj);
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
	XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
	XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

	XMMATRIX shadowTransform = XMLoadFloat4x4(&mShadowTransform);

	XMStoreFloat4x4(&mMainPassCB.View, XMMatrixTranspose(view));
	XMStoreFloat4x4(&mMainPassCB.InvView, XMMatrixTranspose(invView));
	XMStoreFloat4x4(&mMainPassCB.Proj, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&mMainPassCB.InvProj, XMMatrixTranspose(invProj));
	XMStoreFloat4x4(&mMainPassCB.ViewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat4x4(&mMainPassCB.InvViewProj, XMMatrixTranspose(invViewProj));
	XMStoreFloat4x4(&mMainPassCB.ShadowTransform, XMMatrixTranspose(shadowTransform));

	mMainPassCB.EyePosW = mCamera.GetPosition3f();
	mMainPassCB.gTimer = gt.TotalTime();

	mMainPassCB.RenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
	mMainPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
	mMainPassCB.NearZ = 1.0f;
	mMainPassCB.FarZ = 1000.0f;
	mMainPassCB.TotalTime = gt.TotalTime();
	mMainPassCB.DeltaTime = gt.DeltaTime();

	int lightCount = 0;
	//간접광 흉내
	mMainPassCB.AmbientLight = { 0.05f, 0.05f, 0.15f, 1.0f };
	mMainPassCB.Lights[0].Direction = mRotatedLightDirections[0];
	mMainPassCB.Lights[0].Strength = { 0.5f, 0.5f, 0.5f };
	mMainPassCB.Lights[1].Direction = mRotatedLightDirections[1];
	mMainPassCB.Lights[1].Strength = { 0.1f, 0.1f, 0.1f };
	mMainPassCB.Lights[2].Direction = mRotatedLightDirections[2];
	mMainPassCB.Lights[2].Strength = { 0.15f, 0.15f, 0.15f };

	auto& items = mOpaqueRitems[(int)RenderLayer::Item];
	mMainPassCB.Lights[3].FalloffStart = 100;
	mMainPassCB.Lights[3].FalloffEnd = 400;
	mMainPassCB.Lights[3].Position = items[0]->GetPosition();
	mMainPassCB.Lights[3].Strength = { 1.0f, 0.7f, 0.0f };


	mMainPassCB.gFogStart = 600.0f;
	mMainPassCB.gFogRange = 900.0f;

	auto& eplayer = mOpaqueRitems[(int)RenderLayer::Player];
	mMainPassCB.PlayerPos = eplayer[0]->GetPosition();
	mMainPassCB.PlayerPos.y += 200;

	mMainPassCB.Energy = mEnergy;

	auto currPassCB = mCurrFrameResource->PassCB.get();
	currPassCB->CopyData(0, mMainPassCB);
}

void MyScene::UpdateShadowPassCB(const GameTimer& gt)
{
	XMMATRIX view = XMLoadFloat4x4(&mLightView);
	XMMATRIX proj = XMLoadFloat4x4(&mLightProj);

	XMMATRIX viewProj = XMMatrixMultiply(view, proj);
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
	XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
	XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

	UINT w = mShadowMap->Width();
	UINT h = mShadowMap->Height();

	XMStoreFloat4x4(&mShadowPassCB.View, XMMatrixTranspose(view));
	XMStoreFloat4x4(&mShadowPassCB.InvView, XMMatrixTranspose(invView));
	XMStoreFloat4x4(&mShadowPassCB.Proj, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&mShadowPassCB.InvProj, XMMatrixTranspose(invProj));
	XMStoreFloat4x4(&mShadowPassCB.ViewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat4x4(&mShadowPassCB.InvViewProj, XMMatrixTranspose(invViewProj));
	mShadowPassCB.EyePosW = mLightPosW;
	mShadowPassCB.RenderTargetSize = XMFLOAT2((float)w, (float)h);
	mShadowPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / w, 1.0f / h);
	mShadowPassCB.NearZ = mLightNearZ;
	mShadowPassCB.FarZ = mLightFarZ;

	auto currPassCB = mCurrFrameResource->PassCB.get();
	currPassCB->CopyData(1, mShadowPassCB);
}

void MyScene::LoadTextures()
{
	std::vector<std::string> texNames =
	{
		"tileTex",
		"tilenomalTex",
		"robotTex",
		"robot_nomalTex",
		"skyCubeMap",
		"menuTex",
		"startTex",
		"start_clickTex",
		"quitTex",
		"quit_clickTex",
		"ui01Tex",
		"ui02Tex",
		"handLightTex",
		"handLightNomalTex",
		"leverTex",
		"statueTex",
		"statueNomalTex",
		"bricksTex",
		"bricksNomalTex",
		"woodTex",
		"woodNomalTex",
		"flareArrayTex"
	};

	std::vector<std::wstring> texFilenames =
	{
		L"Textures/tile.dds",
		L"Textures/tile_nmap.dds",
		L"Textures/monster.dds",
		L"Textures/monster_nomal.dds",
		L"Textures/desertcube1024.dds",
		L"Textures/menu.dds",
		L"Textures/start.dds",
		L"Textures/start_click.dds",
		L"Textures/quit.dds",
		L"Textures/quit_click.dds",
		L"Textures/ui01.dds",
		L"Textures/ui02.dds",
		L"Textures/handLight.dds",
		L"Textures/handLightNomal.dds",
		L"Textures/gu.dds", //레버
		L"Textures/statue.dds",
		L"Textures/statue_nomal.dds",
		L"Textures/bricks.dds",
		L"Textures/bricksNomal.dds",
		L"Textures/wood.dds",
		L"Textures/woodNomal.dds",
		L"Textures/explosion.dds"
	};

	for (int i = 0; i < (int)texNames.size(); ++i)
	{
		auto texMap = std::make_unique<Texture>();
		texMap->Name = texNames[i];
		texMap->Filename = texFilenames[i];
		ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(),
			mCommandList.Get(), texMap->Filename.c_str(),
			texMap->Resource, texMap->UploadHeap));

		mTextures[texMap->Name] = std::move(texMap);
	}
}

void MyScene::BuildRootSignature()
{
	CD3DX12_DESCRIPTOR_RANGE texTable[4];
	texTable[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0); //기본텍스처
	texTable[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2); //텍스처2
	texTable[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3); //텍스처3
	texTable[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0);//null용 텍스처(슬롯7)
	// Root parameter can be a table, root descriptor or root constants.
	CD3DX12_ROOT_PARAMETER slotRootParameter[8];

	// Perfomance TIP: Order from most frequent to least frequent.
	slotRootParameter[0].InitAsDescriptorTable(1, &texTable[0], D3D12_SHADER_VISIBILITY_PIXEL);
	slotRootParameter[5].InitAsDescriptorTable(1, &texTable[1], D3D12_SHADER_VISIBILITY_PIXEL);
	slotRootParameter[6].InitAsDescriptorTable(1, &texTable[2], D3D12_SHADER_VISIBILITY_PIXEL);
	slotRootParameter[7].InitAsDescriptorTable(1, &texTable[3], D3D12_SHADER_VISIBILITY_PIXEL);

	slotRootParameter[1].InitAsConstantBufferView(0); //버텍스상수
	slotRootParameter[2].InitAsConstantBufferView(1); //상수
	slotRootParameter[3].InitAsConstantBufferView(2); //매터리얼상수
	slotRootParameter[4].InitAsConstantBufferView(3); //본

	auto staticSamplers = GetStaticSamplers();

	// 루트 서명은 루트 매개변수들의 배열이다
	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(8, slotRootParameter, //루트파라메터 사이즈
		(UINT)staticSamplers.size(), staticSamplers.data(),
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
	//상수 버퍼 하나로 구성된 서술자 구간을 카리키는 슬롯 하나로 이루어진 루트 서명을 생성한다.
	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
	{
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
	}
	ThrowIfFailed(hr);

	ThrowIfFailed(md3dDevice->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(mRootSignature.GetAddressOf())));
}

void MyScene::BuildPostProcessRootSignature()
{
	CD3DX12_DESCRIPTOR_RANGE srvTable;
	srvTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

	CD3DX12_DESCRIPTOR_RANGE uavTable;
	uavTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);

	// Root parameter can be a table, root descriptor or root constants.
	CD3DX12_ROOT_PARAMETER slotRootParameter[3];

	// Perfomance TIP: Order from most frequent to least frequent.
	slotRootParameter[0].InitAsConstants(12, 0);
	slotRootParameter[1].InitAsDescriptorTable(1, &srvTable);
	slotRootParameter[2].InitAsDescriptorTable(1, &uavTable);

	// A root signature is an array of root parameters.
	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, slotRootParameter,
		0, nullptr,
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
	{
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
	}
	ThrowIfFailed(hr);

	ThrowIfFailed(md3dDevice->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(mPostProcessRootSignature.GetAddressOf())));
}

void MyScene::BuildPostProcessSobelRootSignature()
{
	CD3DX12_DESCRIPTOR_RANGE srvTable0;
	srvTable0.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

	CD3DX12_DESCRIPTOR_RANGE srvTable1;
	srvTable1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);

	CD3DX12_DESCRIPTOR_RANGE uavTable0;
	uavTable0.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);

	// Root parameter can be a table, root descriptor or root constants.
	CD3DX12_ROOT_PARAMETER slotRootParameter[3];

	// Perfomance TIP: Order from most frequent to least frequent.
	slotRootParameter[0].InitAsDescriptorTable(1, &srvTable0);
	slotRootParameter[1].InitAsDescriptorTable(1, &srvTable1);
	slotRootParameter[2].InitAsDescriptorTable(1, &uavTable0);

	auto staticSamplers = GetStaticSamplers();

	// A root signature is an array of root parameters.
	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, slotRootParameter,
		(UINT)staticSamplers.size(), staticSamplers.data(),
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
	{
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
	}
	ThrowIfFailed(hr);

	ThrowIfFailed(md3dDevice->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(mPostProcessSobelRootSignature.GetAddressOf())));
}

void MyScene::BuildDescriptorHeaps()
{
	int rtvOffset = SwapChainBufferCount;

	const int textureDescriptorCount = 22;
	const int blurDescriptorCount = 4;

	int srvOffset = textureDescriptorCount;
	int sobelSrvOffset = srvOffset + blurDescriptorCount;
	int offscreenSrvOffset = sobelSrvOffset + mSobelFilter->DescriptorCount(); // +1

	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
	srvHeapDesc.NumDescriptors = textureDescriptorCount + blurDescriptorCount +
		mSobelFilter->DescriptorCount() + 1 + 3;
	srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mCbvSrvUavDescriptorHeap)));

	CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(mCbvSrvUavDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

	std::vector<ComPtr<ID3D12Resource>> tex2DList =
	{
		mTextures["tileTex"]->Resource,
		mTextures["tilenomalTex"]->Resource,
		mTextures["robotTex"]->Resource,
		mTextures["robot_nomalTex"]->Resource,
		mTextures["menuTex"]->Resource,
		mTextures["startTex"]->Resource,
		mTextures["start_clickTex"]->Resource,
		mTextures["quitTex"]->Resource,
		mTextures["quit_clickTex"]->Resource,
		mTextures["ui01Tex"]->Resource ,
		mTextures["ui02Tex"]->Resource,
		mTextures["handLightTex"]->Resource,
		mTextures["handLightNomalTex"]->Resource,
		mTextures["leverTex"]->Resource,
		mTextures["statueTex"]->Resource,
		mTextures["statueNomalTex"]->Resource,
		mTextures["bricksTex"]->Resource,
		mTextures["bricksNomalTex"]->Resource,
		mTextures["woodTex"]->Resource,
		mTextures["woodNomalTex"]->Resource,
		mTextures["flareArrayTex"]->Resource
	};
	auto skyCubeMap = mTextures["skyCubeMap"]->Resource;

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

	for (UINT i = 0; i < (UINT)tex2DList.size(); ++i)
	{
		srvDesc.Format = tex2DList[i]->GetDesc().Format;
		srvDesc.Texture2D.MipLevels = tex2DList[i]->GetDesc().MipLevels;
		md3dDevice->CreateShaderResourceView(tex2DList[i].Get(), &srvDesc, hDescriptor);

		// next descriptor
		hDescriptor.Offset(1, mCbvSrvUavDescriptorSize);
	}
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
	srvDesc.TextureCube.MostDetailedMip = 0;
	srvDesc.TextureCube.MipLevels = skyCubeMap->GetDesc().MipLevels;
	srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
	srvDesc.Format = skyCubeMap->GetDesc().Format;
	md3dDevice->CreateShaderResourceView(skyCubeMap.Get(), &srvDesc, hDescriptor);

	mSkyTexHeapIndex = offscreenSrvOffset;
	mShadowMapHeapIndex = mSkyTexHeapIndex + 1;
	mNullCubeSrvIndex = mShadowMapHeapIndex + 1;
	mNullTexSrvIndex = mNullCubeSrvIndex + 1;

	// Fill out the heap with the descriptors to the BlurFilter resources.

	auto srvCpuStart = mCbvSrvUavDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
	auto srvGpuStart = mCbvSrvUavDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
	auto dsvCpuStart = mDsvHeap->GetCPUDescriptorHandleForHeapStart();

	mBlurFilter->BuildDescriptors(
		CD3DX12_CPU_DESCRIPTOR_HANDLE(srvCpuStart, srvOffset, mCbvSrvUavDescriptorSize),
		CD3DX12_GPU_DESCRIPTOR_HANDLE(srvGpuStart, srvOffset, mCbvSrvUavDescriptorSize),
		mCbvSrvUavDescriptorSize);

	mSobelFilter->BuildDescriptors(
		CD3DX12_CPU_DESCRIPTOR_HANDLE(srvCpuStart, sobelSrvOffset, mCbvSrvUavDescriptorSize),
		CD3DX12_GPU_DESCRIPTOR_HANDLE(srvGpuStart, sobelSrvOffset, mCbvSrvUavDescriptorSize),
		mCbvSrvUavDescriptorSize);

	auto rtvCpuStart = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
	mOffscreenRT->BuildDescriptors(
		CD3DX12_CPU_DESCRIPTOR_HANDLE(srvCpuStart, offscreenSrvOffset, mCbvSrvUavDescriptorSize),
		CD3DX12_GPU_DESCRIPTOR_HANDLE(srvGpuStart, offscreenSrvOffset, mCbvSrvUavDescriptorSize),
		CD3DX12_CPU_DESCRIPTOR_HANDLE(rtvCpuStart, rtvOffset, mRtvDescriptorSize));

	///////////////////////////////////////////////////////////////////////////////////////////////////////
	auto nullSrv = CD3DX12_CPU_DESCRIPTOR_HANDLE(srvCpuStart, mNullCubeSrvIndex, mCbvSrvUavDescriptorSize);
	mNullSrv = CD3DX12_GPU_DESCRIPTOR_HANDLE(srvGpuStart, mNullCubeSrvIndex, mCbvSrvUavDescriptorSize);

	md3dDevice->CreateShaderResourceView(nullptr, &srvDesc, nullSrv);
	nullSrv.Offset(1, mCbvSrvUavDescriptorSize);

	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = 1;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
	md3dDevice->CreateShaderResourceView(nullptr, &srvDesc, nullSrv);

	mShadowMap->BuildDescriptors(
		CD3DX12_CPU_DESCRIPTOR_HANDLE(srvCpuStart, mShadowMapHeapIndex, mCbvSrvUavDescriptorSize),
		CD3DX12_GPU_DESCRIPTOR_HANDLE(srvGpuStart, mShadowMapHeapIndex, mCbvSrvUavDescriptorSize),
		CD3DX12_CPU_DESCRIPTOR_HANDLE(dsvCpuStart, 1, mDsvDescriptorSize));
}

void MyScene::BuildShadersAndInputLayout()
{
	const D3D_SHADER_MACRO defines[] =
	{
		"FOG", "1",
		NULL, NULL
	};

	const D3D_SHADER_MACRO uis[] =
	{
		"ALPHA_TEST", "1",
		NULL, NULL
	};

	const D3D_SHADER_MACRO moveui[] =
	{
		"ALPHA_TEST", "1",
		"MOVE", "1",
		NULL, NULL
	};

	const D3D_SHADER_MACRO alphaTestDefines[] =
	{
		"FOG", "1",
		"ALPHA_TEST", "1",
		NULL, NULL
	};

	const D3D_SHADER_MACRO skinnedDefines[] =
	{
		"SKINNED", "1",
		NULL, NULL
	};

	mShaders["standardVS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["skinnedVS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", skinnedDefines, "VS", "vs_5_1");
	mShaders["opaquePS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", defines, "PS", "ps_5_1");

	mShaders["uiVS"] = d3dUtil::CompileShader(L"Shaders\\Ui.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["uiPS"] = d3dUtil::CompileShader(L"Shaders\\Ui.hlsl", nullptr, "PS", "ps_5_1");
	mShaders["uiAlphaTestPS"] = d3dUtil::CompileShader(L"Shaders\\Ui.hlsl", uis, "PS", "ps_5_1");
	mShaders["uiMovePS"] = d3dUtil::CompileShader(L"Shaders\\Ui.hlsl", moveui, "PS", "ps_5_1");

	mShaders["shadowVS"] = d3dUtil::CompileShader(L"Shaders\\Shadows.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["shadowSkinVS"] = d3dUtil::CompileShader(L"Shaders\\Shadows.hlsl", skinnedDefines, "VS", "vs_5_1");
	mShaders["shadowOpaquePS"] = d3dUtil::CompileShader(L"Shaders\\Shadows.hlsl", nullptr, "PS", "ps_5_1");
	mShaders["shadowAlphaTestedPS"] = d3dUtil::CompileShader(L"Shaders\\Shadows.hlsl", alphaTestDefines, "PS", "ps_5_1");

	mShaders["debugVS"] = d3dUtil::CompileShader(L"Shaders\\ShadowDebug.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["debugPS"] = d3dUtil::CompileShader(L"Shaders\\ShadowDebug.hlsl", nullptr, "PS", "ps_5_1");

	mShaders["treeSpriteVS"] = d3dUtil::CompileShader(L"Shaders\\TreeSprite.hlsl", nullptr, "VS", "vs_5_0");
	mShaders["treeSpriteGS"] = d3dUtil::CompileShader(L"Shaders\\TreeSprite.hlsl", nullptr, "GS", "gs_5_0");
	mShaders["treeSpritePS"] = d3dUtil::CompileShader(L"Shaders\\TreeSprite.hlsl", alphaTestDefines, "PS", "ps_5_0");

	mShaders["flareSpriteVS"] = d3dUtil::CompileShader(L"Shaders\\flareSprite.hlsl", nullptr, "VS", "vs_5_0");
	mShaders["flareSpriteGS"] = d3dUtil::CompileShader(L"Shaders\\flareSprite.hlsl", nullptr, "GS", "gs_5_0");
	mShaders["flareSpritePS"] = d3dUtil::CompileShader(L"Shaders\\flareSprite.hlsl", alphaTestDefines, "PS", "ps_5_0");

	mShaders["SkyBoxVS"] = d3dUtil::CompileShader(L"Shaders\\SkyBox.hlsl", nullptr, "VS", "vs_5_0");
	mShaders["SkyBoxPS"] = d3dUtil::CompileShader(L"Shaders\\SkyBox.hlsl", nullptr, "PS", "ps_5_0");

	mShaders["horzBlurCS"] = d3dUtil::CompileShader(L"Shaders\\Blur.hlsl", nullptr, "HorzBlurCS", "cs_5_0");
	mShaders["vertBlurCS"] = d3dUtil::CompileShader(L"Shaders\\Blur.hlsl", nullptr, "VertBlurCS", "cs_5_0");

	mShaders["compositeVS"] = d3dUtil::CompileShader(L"Shaders\\Composite.hlsl", nullptr, "VS", "vs_5_0");
	mShaders["compositePS"] = d3dUtil::CompileShader(L"Shaders\\Composite.hlsl", nullptr, "PS", "ps_5_0");
	mShaders["sobelCS"] = d3dUtil::CompileShader(L"Shaders\\Sobel.hlsl", nullptr, "SobelCS", "cs_5_0");

	mInputLayout =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	//폭발이펙트도 이거씀
	mTreeSpriteInputLayout =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "SIZE", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	mSkinnedInputLayout =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "WEIGHTS", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BONEINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

}

void MyScene::BuildAnimation(const std::string fileName, std::string clipNmae, float loadScale, bool isMap)
{
	auto dummy = std::make_unique<GameObject>();

	dummy->pMesh = playerMesh;
	dummy->boneOffsets = playerboneOffsets;
	dummy->boneIndexToParentIndex = playerboneIndexToParentIndex;
	dummy->boneName = playerboneName;

	dummy->LoadAnimationModel(fileName, loadScale);
	//dummy->LoadGameModel(fileName, loadScale, isMap, true);

	dummy->LoadAnimation(mSkinnedInfo, clipNmae, loadScale);// "AnimStack::Take 001");

	mSkinnedModelInst = std::make_unique<SkinnedModelInstance>();
	mSkinnedModelInst->SkinnedInfo = &mSkinnedInfo;
	mSkinnedModelInst->FinalTransforms.resize(mSkinnedInfo.BoneCount());
	mSkinnedModelInst->ClipName = clipNmae;
	mSkinnedModelInst->TimePos = 0.0f;
	mSkinnedModelInst->SetNowAni("Take001");

	mSkinnedFriendModelInst = std::make_unique<SkinnedModelInstance>();
	mSkinnedFriendModelInst->SkinnedInfo = &mSkinnedInfo;
	mSkinnedFriendModelInst->FinalTransforms.resize(mSkinnedInfo.BoneCount());
	mSkinnedFriendModelInst->ClipName = clipNmae;
	mSkinnedFriendModelInst->TimePos = 0.0f;
	mSkinnedFriendModelInst->SetNowAni("Take001");
	//mSkinnedFriendModelInst->SetNowAni("idle");
}

void MyScene::BuildShapeGeometry()
{
	GeometryGenerator geoGen;
	GeometryGenerator::MeshData box = geoGen.CreateBox(1.0f, 1.0f, 1.0f, 3);
	GeometryGenerator::MeshData grid = geoGen.CreateGrid(200.0f, 200.0f, 60, 60);
	GeometryGenerator::MeshData sphere = geoGen.CreateSphere(0.5f, 20, 20);
	GeometryGenerator::MeshData cylinder = geoGen.CreateCylinder(0.5f, 0.3f, 3.0f, 20, 20);
	GeometryGenerator::MeshData quad = geoGen.CreateQuad(0.0f, 0.0f, 1.0f, 1.0f, 0.0f);

	//
	// We are concatenating all the geometry into one big vertex/index buffer.  So
	// define the regions in the buffer each submesh covers.
	//

	// Cache the vertex offsets to each object in the concatenated vertex buffer.
	UINT boxVertexOffset = 0;
	UINT gridVertexOffset = (UINT)box.Vertices.size();
	UINT sphereVertexOffset = gridVertexOffset + (UINT)grid.Vertices.size();
	UINT cylinderVertexOffset = sphereVertexOffset + (UINT)sphere.Vertices.size();
	UINT quadVertexOffset = cylinderVertexOffset + (UINT)cylinder.Vertices.size();

	// Cache the starting index for each object in the concatenated index buffer.
	UINT boxIndexOffset = 0;
	UINT gridIndexOffset = (UINT)box.Indices32.size();
	UINT sphereIndexOffset = gridIndexOffset + (UINT)grid.Indices32.size();
	UINT cylinderIndexOffset = sphereIndexOffset + (UINT)sphere.Indices32.size();
	UINT quadIndexOffset = cylinderIndexOffset + (UINT)cylinder.Indices32.size();

	SubmeshGeometry boxSubmesh;
	boxSubmesh.IndexCount = (UINT)box.Indices32.size();
	boxSubmesh.StartIndexLocation = boxIndexOffset;
	boxSubmesh.BaseVertexLocation = boxVertexOffset;

	SubmeshGeometry gridSubmesh;
	gridSubmesh.IndexCount = (UINT)grid.Indices32.size();
	gridSubmesh.StartIndexLocation = gridIndexOffset;
	gridSubmesh.BaseVertexLocation = gridVertexOffset;

	SubmeshGeometry sphereSubmesh;
	sphereSubmesh.IndexCount = (UINT)sphere.Indices32.size();
	sphereSubmesh.StartIndexLocation = sphereIndexOffset;
	sphereSubmesh.BaseVertexLocation = sphereVertexOffset;

	SubmeshGeometry cylinderSubmesh;
	cylinderSubmesh.IndexCount = (UINT)cylinder.Indices32.size();
	cylinderSubmesh.StartIndexLocation = cylinderIndexOffset;
	cylinderSubmesh.BaseVertexLocation = cylinderVertexOffset;

	SubmeshGeometry quadSubmesh;
	quadSubmesh.IndexCount = (UINT)quad.Indices32.size();
	quadSubmesh.StartIndexLocation = quadIndexOffset;
	quadSubmesh.BaseVertexLocation = quadVertexOffset;

	//
	// Extract the vertex elements we are interested in and pack the
	// vertices of all the meshes into one vertex buffer.
	//

	auto totalVertexCount =
		box.Vertices.size() +
		grid.Vertices.size() +
		sphere.Vertices.size() +
		cylinder.Vertices.size() +
		quad.Vertices.size();

	std::vector<Vertex> vertices(totalVertexCount);

	UINT k = 0;
	for (size_t i = 0; i < box.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = box.Vertices[i].Position;
		vertices[k].Normal = box.Vertices[i].Normal;
		vertices[k].TexC = box.Vertices[i].TexC;
		vertices[k].TangentU = box.Vertices[i].TangentU;
	}

	for (size_t i = 0; i < grid.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = grid.Vertices[i].Position;
		vertices[k].Normal = grid.Vertices[i].Normal;
		vertices[k].TexC = grid.Vertices[i].TexC;
		vertices[k].TangentU = grid.Vertices[i].TangentU;
	}

	for (size_t i = 0; i < sphere.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = sphere.Vertices[i].Position;
		vertices[k].Normal = sphere.Vertices[i].Normal;
		vertices[k].TexC = sphere.Vertices[i].TexC;
		vertices[k].TangentU = sphere.Vertices[i].TangentU;
	}

	for (size_t i = 0; i < cylinder.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = cylinder.Vertices[i].Position;
		vertices[k].Normal = cylinder.Vertices[i].Normal;
		vertices[k].TexC = cylinder.Vertices[i].TexC;
		vertices[k].TangentU = cylinder.Vertices[i].TangentU;
	}

	for (int i = 0; i < quad.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = quad.Vertices[i].Position;
		vertices[k].Normal = quad.Vertices[i].Normal;
		vertices[k].TexC = quad.Vertices[i].TexC;
		vertices[k].TangentU = quad.Vertices[i].TangentU;
	}

	std::vector<std::uint16_t> indices;
	indices.insert(indices.end(), std::begin(box.GetIndices16()), std::end(box.GetIndices16()));
	indices.insert(indices.end(), std::begin(grid.GetIndices16()), std::end(grid.GetIndices16()));
	indices.insert(indices.end(), std::begin(sphere.GetIndices16()), std::end(sphere.GetIndices16()));
	indices.insert(indices.end(), std::begin(cylinder.GetIndices16()), std::end(cylinder.GetIndices16()));
	indices.insert(indices.end(), std::begin(quad.GetIndices16()), std::end(quad.GetIndices16()));

	const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
	const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

	auto geo = std::make_unique<MeshGeometry>();
	geo->Name = "shapeGeo";

	ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
	CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);

	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

	geo->VertexByteStride = sizeof(Vertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat = DXGI_FORMAT_R16_UINT;
	geo->IndexBufferByteSize = ibByteSize;

	geo->DrawArgs["box"] = boxSubmesh;
	geo->DrawArgs["grid"] = gridSubmesh;
	geo->DrawArgs["sphere"] = sphereSubmesh;
	geo->DrawArgs["cylinder"] = cylinderSubmesh;
	geo->DrawArgs["quad"] = quadSubmesh;

	mGeometries[geo->Name] = std::move(geo);
}

void MyScene::BuildFlareSpritesGeometry()
{
	struct FlareSpriteVertex
	{
		XMFLOAT3 Pos;
		XMFLOAT2 Size;
	};

	static const int treeCount = 1;
	std::array<FlareSpriteVertex, treeCount> vertices;
	for (int i = 0; i < treeCount; ++i) {
		vertices[i].Pos = XMFLOAT3(0, 0, 0);
		vertices[i].Size = XMFLOAT2(300.0f, 300.0f);
	}
	const int indicesSize = 1;
	int count = 0;
	std::array<std::uint16_t, indicesSize> indices;

	for (int i = 0; i < indicesSize; ++i)
		indices[i] = i;

	const UINT vbByteSize = (UINT)vertices.size() * sizeof(FlareSpriteVertex);
	const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

	auto geo = std::make_unique<MeshGeometry>();
	geo->Name = "flareSpritesGeo";

	ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
	CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);

	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

	geo->VertexByteStride = sizeof(FlareSpriteVertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat = DXGI_FORMAT_R16_UINT;
	geo->IndexBufferByteSize = ibByteSize;

	SubmeshGeometry submesh;
	submesh.IndexCount = (UINT)indices.size();
	submesh.StartIndexLocation = 0;
	submesh.BaseVertexLocation = 0;

	geo->DrawArgs["point"] = submesh;

	mGeometries["flareSpritesGeo"] = std::move(geo);
}

void MyScene::BuildCollBoxGeometry(Aabb colbox, const std::string geoName, const std::string meshName, bool isTile)
{
	GeometryGenerator geoGen;
	XMFLOAT3 *_box;
	_box = colbox.GetAabbBox();
	GeometryGenerator::MeshData box = geoGen.CreateBox(_box[1].x * 2, _box[1].y * 2, _box[1].z * 2);

	UINT robotVertexOffset = 0;
	UINT robotIndexOffset = 0;

	SubmeshGeometry robotSubmesh;
	robotSubmesh.IndexCount = (UINT)box.Indices32.size();
	robotSubmesh.StartIndexLocation = robotIndexOffset;
	robotSubmesh.BaseVertexLocation = robotVertexOffset;

	size_t totalVertexCount = 0;
	totalVertexCount += box.Vertices.size();

	UINT k = 0;
	std::vector<Vertex> vertices(totalVertexCount);

	for (size_t i = 0; i < box.Vertices.size(); ++i, ++k)
	{
		auto& p = box.Vertices[i].Position;
		vertices[k].Pos = p;
	}


	std::vector<std::uint32_t> indices;
	indices.insert(indices.end(), std::begin(box.Indices32), std::end(box.Indices32));

	const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
	const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint32_t);

	auto geo = std::make_unique<MeshGeometry>();
	geo->Name = geoName;

	ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
	CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);

	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

	geo->VertexByteStride = sizeof(Vertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat = DXGI_FORMAT_R32_UINT;
	geo->IndexBufferByteSize = ibByteSize;

	geo->DrawArgs[meshName] = robotSubmesh;

	mGeometries[geo->Name] = std::move(geo);
}

void MyScene::BuildFbxGeometry(const std::string fileName, const std::string geoName, const std::string meshName, float loadScale, bool isMap, bool hasAniBone)
{
	GeometryGenerator geoGen;
	auto dummy = std::make_unique<GameObject>();
	dummy->LoadGameModel(fileName, loadScale, isMap, hasAniBone);

	if (hasAniBone) {
		playerMesh = dummy->pMesh;
		playerboneOffsets = dummy->boneOffsets;
		playerboneIndexToParentIndex = dummy->boneIndexToParentIndex;
		playerboneName = dummy->boneName;
	}

	GeometryGenerator::SkinnedMeshData *robot = dummy->GetSkinMeshData();
	int meshSize = dummy->meshSize;

	UINT robotVertexOffset = 0;
	UINT robotIndexOffset = 0;

	SubmeshGeometry robotSubmesh;
	for (int i = 0; i < meshSize; ++i) {
		robotSubmesh.IndexCount += (UINT)robot[i].Indices32.size();
	}
	robotSubmesh.StartIndexLocation = robotIndexOffset;
	robotSubmesh.BaseVertexLocation = robotVertexOffset;

	size_t totalVertexCount = 0;
	for (int i = 0; i < meshSize; ++i) {
		totalVertexCount += robot[i].Vertices.size();
	}

	UINT k = 0;
	std::vector<SkinnedVertex> vertices(totalVertexCount);

	for (int z = 0; z < meshSize; ++z) {

		for (size_t i = 0; i < robot[z].Vertices.size(); ++i, ++k)
		{
			auto& p = robot[z].Vertices[i].Position;
			vertices[k].Pos = p;
			vertices[k].Normal = robot[z].Vertices[i].Normal;
			vertices[k].TexC = robot[z].Vertices[i].TexC;
			vertices[k].TangentU = robot[z].Vertices[i].TangentU;
			vertices[k].BoneWeights = robot[z].Vertices[i].BoneWeights;
			vertices[k].BoneIndices[0] = robot[z].Vertices[i].BoneIndices[0];
			vertices[k].BoneIndices[1] = robot[z].Vertices[i].BoneIndices[1];
			vertices[k].BoneIndices[2] = robot[z].Vertices[i].BoneIndices[2];
			vertices[k].BoneIndices[3] = robot[z].Vertices[i].BoneIndices[3];
		}

	}

	std::vector<std::uint32_t> indices;
	for (int i = 0; i < meshSize; ++i) {
		indices.insert(indices.end(), std::begin(robot[i].Indices32), std::end(robot[i].Indices32));
	}

	const UINT vbByteSize = (UINT)vertices.size() * sizeof(SkinnedVertex);
	const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint32_t);

	auto geo = std::make_unique<MeshGeometry>();
	geo->Name = geoName;

	ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
	CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);

	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

	geo->VertexByteStride = sizeof(SkinnedVertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat = DXGI_FORMAT_R32_UINT;
	geo->IndexBufferByteSize = ibByteSize;

	geo->DrawArgs[meshName] = robotSubmesh;

	mGeometries[geo->Name] = std::move(geo);
	//바운드박스정보도 로드
	mBounds[meshName] = dummy->bounds;
}

void MyScene::BuildPSOs()
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC opaquePsoDesc;

	//
	// PSO for opaque objects.
	//
	ZeroMemory(&opaquePsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	opaquePsoDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
	opaquePsoDesc.pRootSignature = mRootSignature.Get();
	opaquePsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["standardVS"]->GetBufferPointer()),
		mShaders["standardVS"]->GetBufferSize()
	};
	opaquePsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["opaquePS"]->GetBufferPointer()),
		mShaders["opaquePS"]->GetBufferSize()
	};
	opaquePsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	opaquePsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	opaquePsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	opaquePsoDesc.SampleMask = UINT_MAX;
	opaquePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	opaquePsoDesc.NumRenderTargets = 1;
	opaquePsoDesc.RTVFormats[0] = mBackBufferFormat;
	opaquePsoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
	opaquePsoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	opaquePsoDesc.DSVFormat = mDepthStencilFormat;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaquePsoDesc, IID_PPV_ARGS(&mPSOs["opaque"])));

	//
	// PSO for shadow map pass.
	//
	D3D12_GRAPHICS_PIPELINE_STATE_DESC smapPsoDesc = opaquePsoDesc;
	smapPsoDesc.RasterizerState.DepthBias = 100000;
	smapPsoDesc.RasterizerState.DepthBiasClamp = 0.0f;
	smapPsoDesc.RasterizerState.SlopeScaledDepthBias = 1.0f;
	smapPsoDesc.pRootSignature = mRootSignature.Get();
	smapPsoDesc.InputLayout = { mSkinnedInputLayout.data(), (UINT)mSkinnedInputLayout.size() };
	smapPsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["shadowVS"]->GetBufferPointer()),
		mShaders["shadowVS"]->GetBufferSize()
	};
	smapPsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["shadowOpaquePS"]->GetBufferPointer()),
		mShaders["shadowOpaquePS"]->GetBufferSize()
	};

	// Shadow map pass does not have a render target.
	smapPsoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
	smapPsoDesc.NumRenderTargets = 0;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&smapPsoDesc, IID_PPV_ARGS(&mPSOs["shadow_opaque"])));

	//
	// PSO for shadow map pass (animation).
	//
	D3D12_GRAPHICS_PIPELINE_STATE_DESC ssmapPsoDesc = opaquePsoDesc;
	ssmapPsoDesc.RasterizerState.DepthBias = 100000;
	ssmapPsoDesc.RasterizerState.DepthBiasClamp = 0.0f;
	ssmapPsoDesc.RasterizerState.SlopeScaledDepthBias = 1.0f;
	ssmapPsoDesc.pRootSignature = mRootSignature.Get();
	ssmapPsoDesc.InputLayout = { mSkinnedInputLayout.data(), (UINT)mSkinnedInputLayout.size() };
	ssmapPsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["shadowSkinVS"]->GetBufferPointer()),
		mShaders["shadowSkinVS"]->GetBufferSize()
	};
	ssmapPsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["shadowOpaquePS"]->GetBufferPointer()),
		mShaders["shadowOpaquePS"]->GetBufferSize()
	};

	// Shadow map pass does not have a render target.
	ssmapPsoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
	ssmapPsoDesc.NumRenderTargets = 0;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&ssmapPsoDesc, IID_PPV_ARGS(&mPSOs["shadow_skin_opaque"])));

	//
	// PSO for debug layer.
	//
	D3D12_GRAPHICS_PIPELINE_STATE_DESC debugPsoDesc = opaquePsoDesc;
	debugPsoDesc.pRootSignature = mRootSignature.Get();
	debugPsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["debugVS"]->GetBufferPointer()),
		mShaders["debugVS"]->GetBufferSize()
	};
	debugPsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["debugPS"]->GetBufferPointer()),
		mShaders["debugPS"]->GetBufferSize()
	};
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&debugPsoDesc, IID_PPV_ARGS(&mPSOs["debug"])));

	//
	// PSO for ui layer.
	//
	D3D12_GRAPHICS_PIPELINE_STATE_DESC uiPsoDesc = opaquePsoDesc;
	uiPsoDesc.pRootSignature = mRootSignature.Get();
	uiPsoDesc.DepthStencilState.DepthEnable = false;
	uiPsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["uiVS"]->GetBufferPointer()),
		mShaders["uiVS"]->GetBufferSize()
	};
	uiPsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["uiPS"]->GetBufferPointer()),
		mShaders["uiPS"]->GetBufferSize()
	};
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&uiPsoDesc, IID_PPV_ARGS(&mPSOs["ui"])));

	D3D12_GRAPHICS_PIPELINE_STATE_DESC uiApPsoDesc = uiPsoDesc;
	uiApPsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["uiAlphaTestPS"]->GetBufferPointer()),
		mShaders["uiAlphaTestPS"]->GetBufferSize()
	};
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&uiApPsoDesc, IID_PPV_ARGS(&mPSOs["uiAp"])));

	D3D12_GRAPHICS_PIPELINE_STATE_DESC uiMovePsoDesc = uiPsoDesc;
	uiMovePsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["uiMovePS"]->GetBufferPointer()),
		mShaders["uiMovePS"]->GetBufferSize()
	};
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&uiMovePsoDesc, IID_PPV_ARGS(&mPSOs["uiMove"])));
	//
	// PSO for transparent objects
	//

	D3D12_GRAPHICS_PIPELINE_STATE_DESC transparentPsoDesc = opaquePsoDesc;

	D3D12_RENDER_TARGET_BLEND_DESC transparencyBlendDesc;
	transparencyBlendDesc.BlendEnable = true;
	transparencyBlendDesc.LogicOpEnable = false;
	transparencyBlendDesc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
	transparencyBlendDesc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	transparencyBlendDesc.BlendOp = D3D12_BLEND_OP_ADD;
	transparencyBlendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
	transparencyBlendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
	transparencyBlendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	transparencyBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
	transparencyBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

	transparentPsoDesc.BlendState.RenderTarget[0] = transparencyBlendDesc;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&transparentPsoDesc, IID_PPV_ARGS(&mPSOs["transparent"])));

	//
	// PSO for opaque wireframe objects.
	//

	D3D12_GRAPHICS_PIPELINE_STATE_DESC opaqueWireframePsoDesc = opaquePsoDesc;
	opaqueWireframePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaqueWireframePsoDesc, IID_PPV_ARGS(&mPSOs["opaque_wireframe"])));

	//
	// 그리드
	//
	D3D12_GRAPHICS_PIPELINE_STATE_DESC gridPsoDesc = opaquePsoDesc;
	gridPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&gridPsoDesc, IID_PPV_ARGS(&mPSOs["grid"])));

	//
	// PSO for tree sprites
	//
	D3D12_GRAPHICS_PIPELINE_STATE_DESC treeSpritePsoDesc = opaquePsoDesc;
	//treeSpritePsoDesc.pRootSignature = mRootSignatureForTrees.Get();
	treeSpritePsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["treeSpriteVS"]->GetBufferPointer()),
		mShaders["treeSpriteVS"]->GetBufferSize()
	};
	treeSpritePsoDesc.GS =
	{
		reinterpret_cast<BYTE*>(mShaders["treeSpriteGS"]->GetBufferPointer()),
		mShaders["treeSpriteGS"]->GetBufferSize()
	};
	treeSpritePsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["treeSpritePS"]->GetBufferPointer()),
		mShaders["treeSpritePS"]->GetBufferSize()
	};
	treeSpritePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
	treeSpritePsoDesc.InputLayout = { mTreeSpriteInputLayout.data(), (UINT)mTreeSpriteInputLayout.size() };
	treeSpritePsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&treeSpritePsoDesc, IID_PPV_ARGS(&mPSOs["treeSprites"])));

	//
	// PSO for flare sprites
	//
	D3D12_GRAPHICS_PIPELINE_STATE_DESC flareSpritePsoDesc = opaquePsoDesc;
	flareSpritePsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["flareSpriteVS"]->GetBufferPointer()),
		mShaders["flareSpriteVS"]->GetBufferSize()
	};
	flareSpritePsoDesc.GS =
	{
		reinterpret_cast<BYTE*>(mShaders["flareSpriteGS"]->GetBufferPointer()),
		mShaders["flareSpriteGS"]->GetBufferSize()
	};
	flareSpritePsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["flareSpritePS"]->GetBufferPointer()),
		mShaders["flareSpritePS"]->GetBufferSize()
	};
	flareSpritePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
	flareSpritePsoDesc.InputLayout = { mTreeSpriteInputLayout.data(), (UINT)mTreeSpriteInputLayout.size() };
	flareSpritePsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&flareSpritePsoDesc, IID_PPV_ARGS(&mPSOs["flareSprites"])));

	//
	// PSO for horizontal blur
	//
	D3D12_COMPUTE_PIPELINE_STATE_DESC horzBlurPSO = {};
	horzBlurPSO.pRootSignature = mPostProcessRootSignature.Get();
	horzBlurPSO.CS =
	{
		reinterpret_cast<BYTE*>(mShaders["horzBlurCS"]->GetBufferPointer()),
		mShaders["horzBlurCS"]->GetBufferSize()
	};
	horzBlurPSO.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
	ThrowIfFailed(md3dDevice->CreateComputePipelineState(&horzBlurPSO, IID_PPV_ARGS(&mPSOs["horzBlur"])));

	//
	// PSO for vertical blur
	//
	D3D12_COMPUTE_PIPELINE_STATE_DESC vertBlurPSO = {};
	vertBlurPSO.pRootSignature = mPostProcessRootSignature.Get();
	vertBlurPSO.CS =
	{
		reinterpret_cast<BYTE*>(mShaders["vertBlurCS"]->GetBufferPointer()),
		mShaders["vertBlurCS"]->GetBufferSize()
	};
	vertBlurPSO.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
	ThrowIfFailed(md3dDevice->CreateComputePipelineState(&vertBlurPSO, IID_PPV_ARGS(&mPSOs["vertBlur"])));

	//
	// PSO for compositing post process
	//
	D3D12_GRAPHICS_PIPELINE_STATE_DESC compositePSO = opaquePsoDesc;
	compositePSO.pRootSignature = mPostProcessSobelRootSignature.Get();

	// Disable depth test.
	compositePSO.DepthStencilState.DepthEnable = false;
	compositePSO.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	compositePSO.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;

	compositePSO.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["compositeVS"]->GetBufferPointer()),
		mShaders["compositeVS"]->GetBufferSize()
	};
	compositePSO.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["compositePS"]->GetBufferPointer()),
		mShaders["compositePS"]->GetBufferSize()
	};
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&compositePSO, IID_PPV_ARGS(&mPSOs["composite"])));

	//
	// PSO for sobel
	//
	D3D12_COMPUTE_PIPELINE_STATE_DESC sobelPSO = {};
	sobelPSO.pRootSignature = mPostProcessSobelRootSignature.Get();
	sobelPSO.CS =
	{
		reinterpret_cast<BYTE*>(mShaders["sobelCS"]->GetBufferPointer()),
		mShaders["sobelCS"]->GetBufferSize()
	};
	sobelPSO.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
	ThrowIfFailed(md3dDevice->CreateComputePipelineState(&sobelPSO, IID_PPV_ARGS(&mPSOs["sobel"])));

	//
	// PSO for skinned pass.
	//
	D3D12_GRAPHICS_PIPELINE_STATE_DESC skinnedOpaquePsoDesc = opaquePsoDesc;
	skinnedOpaquePsoDesc.InputLayout = { mSkinnedInputLayout.data(), (UINT)mSkinnedInputLayout.size() };
	skinnedOpaquePsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["skinnedVS"]->GetBufferPointer()),
		mShaders["skinnedVS"]->GetBufferSize()
	};
	skinnedOpaquePsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["opaquePS"]->GetBufferPointer()),
		mShaders["opaquePS"]->GetBufferSize()
	};
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&skinnedOpaquePsoDesc, IID_PPV_ARGS(&mPSOs["skinnedOpaque"])));

	//
	// PSO for sky.
	//
	D3D12_GRAPHICS_PIPELINE_STATE_DESC skyPsoDesc = opaquePsoDesc;

	// The camera is inside the sky sphere, so just turn off culling.
	skyPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

	// Make sure the depth function is LESS_EQUAL and not just LESS.  
	// Otherwise, the normalized depth values at z = 1 (NDC) will 
	// fail the depth test if the depth buffer was cleared to 1.
	skyPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	skyPsoDesc.pRootSignature = mRootSignature.Get();
	skyPsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["SkyBoxVS"]->GetBufferPointer()),
		mShaders["SkyBoxVS"]->GetBufferSize()
	};
	skyPsoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mShaders["SkyBoxPS"]->GetBufferPointer()),
		mShaders["SkyBoxPS"]->GetBufferSize()
	};
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&skyPsoDesc, IID_PPV_ARGS(&mPSOs["sky"])));
}

void MyScene::BuildFrameResources()
{
	mFrameResources.clear();
	for (int i = 0; i < gNumFrameResources; ++i)
	{
		mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(),
			2, (UINT)mAllRitems.size(), 2, (UINT)mMaterials.size()));

	}
}

void MyScene::BuildMaterials()
{
	int matIndex = 0;
	auto rand = std::make_unique<Material>();
	rand->Name = "rand";
	rand->MatCBIndex = matIndex;
	rand->DiffuseSrvHeapIndex = matIndex++;
	rand->DiffuseAlbedo = XMFLOAT4(1, 1.0f, 1, 1.0f);
	rand->FresnelR0 = XMFLOAT3(0.01f, 0.01f, 0.01f);
	rand->Roughness = 0.9f;

	auto gu = std::make_unique<Material>();
	gu->Name = "rand_nomal";
	gu->MatCBIndex = matIndex;
	gu->DiffuseSrvHeapIndex = matIndex++;
	gu->DiffuseAlbedo = XMFLOAT4(1, 1, 1, 1);
	gu->FresnelR0 = XMFLOAT3(0.85f, 0.85f, 0.85f);
	gu->Roughness = 0.5f;

	auto robot = std::make_unique<Material>();
	robot->Name = "robot";
	robot->MatCBIndex = matIndex;
	robot->DiffuseSrvHeapIndex = matIndex++;

	auto robot_nomal = std::make_unique<Material>();
	robot_nomal->Name = "robot_nomal";
	robot_nomal->MatCBIndex = matIndex;
	robot_nomal->DiffuseSrvHeapIndex = matIndex++;

	auto menu = std::make_unique<Material>();
	menu->Name = "menu";
	menu->MatCBIndex = matIndex;
	menu->DiffuseSrvHeapIndex = matIndex++;

	auto start = std::make_unique<Material>();
	start->Name = "start";
	start->MatCBIndex = matIndex;
	start->DiffuseSrvHeapIndex = matIndex++;
	auto start_click = std::make_unique<Material>();
	start_click->Name = "start_click";
	start_click->MatCBIndex = matIndex;
	start_click->DiffuseSrvHeapIndex = matIndex++;
	auto quit = std::make_unique<Material>();
	quit->Name = "quit";
	quit->MatCBIndex = matIndex;
	quit->DiffuseSrvHeapIndex = matIndex++;
	auto quit_click = std::make_unique<Material>();
	quit_click->Name = "quit_click";
	quit_click->MatCBIndex = matIndex;
	quit_click->DiffuseSrvHeapIndex = matIndex++;

	auto ui01 = std::make_unique<Material>();
	ui01->Name = "ui01";
	ui01->MatCBIndex = matIndex;
	ui01->DiffuseSrvHeapIndex = matIndex++;
	auto ui02 = std::make_unique<Material>();
	ui02->Name = "ui02";
	ui02->MatCBIndex = matIndex;
	ui02->DiffuseSrvHeapIndex = matIndex++;
	auto handLight = std::make_unique<Material>();
	handLight->Name = "handLight";
	handLight->MatCBIndex = matIndex;
	handLight->DiffuseSrvHeapIndex = matIndex++;
	auto handLightNomal = std::make_unique<Material>();
	handLightNomal->Name = "handLightNomal";
	handLightNomal->MatCBIndex = matIndex;
	handLightNomal->DiffuseSrvHeapIndex = matIndex++;
	auto lever = std::make_unique<Material>();
	lever->Name = "lever";
	lever->MatCBIndex = matIndex;
	lever->DiffuseSrvHeapIndex = matIndex++;

	auto statue = std::make_unique<Material>();
	statue->Name = "statue";
	statue->MatCBIndex = matIndex;
	statue->DiffuseSrvHeapIndex = matIndex++;
	auto statueNomal = std::make_unique<Material>();
	statueNomal->Name = "statueNomal";
	statueNomal->MatCBIndex = matIndex;
	statueNomal->DiffuseSrvHeapIndex = matIndex++;

	auto bricks = std::make_unique<Material>();
	bricks->Name = "bricks";
	bricks->MatCBIndex = matIndex;
	bricks->DiffuseSrvHeapIndex = matIndex++;
	auto bricksNomal = std::make_unique<Material>();
	bricksNomal->Name = "bricksNomal";
	bricksNomal->MatCBIndex = matIndex;
	bricksNomal->DiffuseSrvHeapIndex = matIndex++;

	auto wood = std::make_unique<Material>();
	wood->Name = "wood";
	wood->MatCBIndex = matIndex;
	wood->DiffuseSrvHeapIndex = matIndex++;
	auto woodNomal = std::make_unique<Material>();
	woodNomal->Name = "woodNomal";
	woodNomal->MatCBIndex = matIndex;
	woodNomal->DiffuseSrvHeapIndex = matIndex++;

	auto flareSprites = std::make_unique<Material>();
	flareSprites->Name = "flareSprites";
	flareSprites->MatCBIndex = matIndex;
	flareSprites->DiffuseSrvHeapIndex = matIndex++;
	flareSprites->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	flareSprites->FresnelR0 = XMFLOAT3(0.01f, 0.01f, 0.01f);
	flareSprites->Roughness = 0.125f;

	auto sky = std::make_unique<Material>();
	sky->Name = "sky";
	sky->MatCBIndex = matIndex;
	sky->DiffuseSrvHeapIndex = matIndex++;

	mMaterials["rand"] = std::move(rand);
	mMaterials["rand_nomal"] = std::move(gu);
	mMaterials["robot"] = std::move(robot);
	mMaterials["robot_nomal"] = std::move(robot_nomal);

	mMaterials["menu"] = std::move(menu);
	mMaterials["start"] = std::move(start);
	mMaterials["start_click"] = std::move(start_click);
	mMaterials["quit"] = std::move(quit);
	mMaterials["quit_click"] = std::move(quit_click);
	mMaterials["ui01"] = std::move(ui01);
	mMaterials["ui02"] = std::move(ui02);

	mMaterials["handLight"] = std::move(handLight);
	mMaterials["handLightNomal"] = std::move(handLightNomal);
	mMaterials["lever"] = std::move(lever);

	mMaterials["statue"] = std::move(statue);
	mMaterials["statueNomal"] = std::move(statueNomal);

	mMaterials["bricks"] = std::move(bricks);
	mMaterials["bricksNomal"] = std::move(bricksNomal);
	mMaterials["wood"] = std::move(wood);
	mMaterials["woodNomal"] = std::move(woodNomal);

	mMaterials["flareSprites"] = std::move(flareSprites);

	mMaterials["sky"] = std::move(sky);
}

void MyScene::BuildGameObjects()
{
	int objIndex = 0;
	//--------------------------------- MENU OBJs -----------------------------------//
	{
		auto menu = std::make_unique<GameObject>();
		XMStoreFloat4x4(&menu->World, XMMatrixScaling(mClientWidth, mClientHeight, 1.0f)*XMMatrixTranslation(-mClientWidth / 2, mClientHeight / 2, 0.0f));
		menu->TexTransform = MathHelper::Identity4x4();
		menu->ObjCBIndex = objIndex++;
		menu->Mat = mMaterials["menu"].get();
		menu->Geo = mGeometries["shapeGeo"].get();
		menu->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		menu->IndexCount = menu->Geo->DrawArgs["quad"].IndexCount;
		menu->StartIndexLocation = menu->Geo->DrawArgs["quad"].StartIndexLocation;
		menu->BaseVertexLocation = menu->Geo->DrawArgs["quad"].BaseVertexLocation;
		mOpaqueRitems[(int)RenderLayer::Menu].push_back(menu.get());
		mAllRitems.push_back(std::move(menu));

		auto start = std::make_unique<GameObject>();
		XMStoreFloat4x4(&start->World, XMMatrixScaling(1, 1, 1.0f));
		start->TexTransform = MathHelper::Identity4x4();
		start->ObjCBIndex = objIndex++;
		start->Mat = mMaterials["start"].get();
		start->Geo = mGeometries["shapeGeo"].get();
		start->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		start->IndexCount = start->Geo->DrawArgs["quad"].IndexCount;
		start->StartIndexLocation = start->Geo->DrawArgs["quad"].StartIndexLocation;
		start->BaseVertexLocation = start->Geo->DrawArgs["quad"].BaseVertexLocation;
		mOpaqueRitems[(int)RenderLayer::MenuButton].push_back(start.get());
		mAllRitems.push_back(std::move(start));

		auto start_click = std::make_unique<GameObject>();
		XMStoreFloat4x4(&start_click->World, XMMatrixScaling(1, 1, 1.0f));
		start_click->TexTransform = MathHelper::Identity4x4();
		start_click->ObjCBIndex = objIndex++;
		start_click->Mat = mMaterials["start_click"].get();
		start_click->Geo = mGeometries["shapeGeo"].get();
		start_click->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		start_click->IndexCount = start_click->Geo->DrawArgs["quad"].IndexCount;
		start_click->StartIndexLocation = start_click->Geo->DrawArgs["quad"].StartIndexLocation;
		start_click->BaseVertexLocation = start_click->Geo->DrawArgs["quad"].BaseVertexLocation;
		mOpaqueRitems[(int)RenderLayer::MenuButton].push_back(start_click.get());
		mAllRitems.push_back(std::move(start_click));

		auto quit = std::make_unique<GameObject>();
		XMStoreFloat4x4(&quit->World, XMMatrixScaling(1, 1, 1.0f));
		quit->TexTransform = MathHelper::Identity4x4();
		quit->ObjCBIndex = objIndex++;
		quit->Mat = mMaterials["quit"].get();
		quit->Geo = mGeometries["shapeGeo"].get();
		quit->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		quit->IndexCount = quit->Geo->DrawArgs["quad"].IndexCount;
		quit->StartIndexLocation = quit->Geo->DrawArgs["quad"].StartIndexLocation;
		quit->BaseVertexLocation = quit->Geo->DrawArgs["quad"].BaseVertexLocation;
		mOpaqueRitems[(int)RenderLayer::MenuButton].push_back(quit.get());
		mAllRitems.push_back(std::move(quit));

		auto quit_click = std::make_unique<GameObject>();
		XMStoreFloat4x4(&quit_click->World, XMMatrixScaling(1, 1, 1.0f));
		quit_click->TexTransform = MathHelper::Identity4x4();
		quit_click->ObjCBIndex = objIndex++;
		quit_click->Mat = mMaterials["quit_click"].get();
		quit_click->Geo = mGeometries["shapeGeo"].get();
		quit_click->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		quit_click->IndexCount = quit_click->Geo->DrawArgs["quad"].IndexCount;
		quit_click->StartIndexLocation = quit_click->Geo->DrawArgs["quad"].StartIndexLocation;
		quit_click->BaseVertexLocation = quit_click->Geo->DrawArgs["quad"].BaseVertexLocation;
		mOpaqueRitems[(int)RenderLayer::MenuButton].push_back(quit_click.get());
		mAllRitems.push_back(std::move(quit_click));
	}
	//-----------------------------------  UIs  ------------------------------------//
	{
		auto baseUi = std::make_unique<GameObject>();
		XMStoreFloat4x4(&baseUi->World, XMMatrixScaling(1, 1, 1.0f));
		baseUi->TexTransform = MathHelper::Identity4x4();
		baseUi->ObjCBIndex = objIndex++;
		baseUi->Mat = mMaterials["ui01"].get();
		baseUi->Geo = mGeometries["shapeGeo"].get();
		baseUi->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		baseUi->IndexCount = baseUi->Geo->DrawArgs["quad"].IndexCount;
		baseUi->StartIndexLocation = baseUi->Geo->DrawArgs["quad"].StartIndexLocation;
		baseUi->BaseVertexLocation = baseUi->Geo->DrawArgs["quad"].BaseVertexLocation;
		mOpaqueRitems[(int)RenderLayer::BaseUI].push_back(baseUi.get());
		mAllRitems.push_back(std::move(baseUi));

		auto moveUi = std::make_unique<GameObject>();
		XMStoreFloat4x4(&moveUi->World, XMMatrixScaling(1, 1, 1.0f));
		moveUi->TexTransform = MathHelper::Identity4x4();
		moveUi->ObjCBIndex = objIndex++;
		moveUi->Mat = mMaterials["ui02"].get();
		moveUi->Geo = mGeometries["shapeGeo"].get();
		moveUi->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		moveUi->IndexCount = moveUi->Geo->DrawArgs["quad"].IndexCount;
		moveUi->StartIndexLocation = moveUi->Geo->DrawArgs["quad"].StartIndexLocation;
		moveUi->BaseVertexLocation = moveUi->Geo->DrawArgs["quad"].BaseVertexLocation;
		mOpaqueRitems[(int)RenderLayer::MoveUI].push_back(moveUi.get());
		mAllRitems.push_back(std::move(moveUi));
	}
	//-----------------------------------  Games  ------------------------------------//
	{
		//지금쓸모없음
		{
			auto skyRitem = std::make_unique<GameObject>();
			XMStoreFloat4x4(&skyRitem->World, XMMatrixScaling(5000.0f, 5000.0f, 5000.0f));
			skyRitem->TexTransform = MathHelper::Identity4x4();
			skyRitem->ObjCBIndex = objIndex++;
			skyRitem->Mat = mMaterials["sky"].get();
			skyRitem->Geo = mGeometries["shapeGeo"].get();
			skyRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
			skyRitem->IndexCount = skyRitem->Geo->DrawArgs["sphere"].IndexCount;
			skyRitem->StartIndexLocation = skyRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
			skyRitem->BaseVertexLocation = skyRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;

			mOpaqueRitems[(int)RenderLayer::SkyBox].push_back(skyRitem.get());
			mAllRitems.push_back(std::move(skyRitem));

			auto quadRitem = std::make_unique<GameObject>();
			quadRitem->World = MathHelper::Identity4x4();
			quadRitem->TexTransform = MathHelper::Identity4x4();
			quadRitem->ObjCBIndex = objIndex++;
			quadRitem->Mat = mMaterials["rand"].get();
			quadRitem->Geo = mGeometries["shapeGeo"].get();
			quadRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
			quadRitem->IndexCount = quadRitem->Geo->DrawArgs["quad"].IndexCount;
			quadRitem->StartIndexLocation = quadRitem->Geo->DrawArgs["quad"].StartIndexLocation;
			quadRitem->BaseVertexLocation = quadRitem->Geo->DrawArgs["quad"].BaseVertexLocation;

			mOpaqueRitems[(int)RenderLayer::Debug].push_back(quadRitem.get());
			mAllRitems.push_back(std::move(quadRitem));
		}

		///////////////////////////////////////////  맵무새 /////////////////////////////////////////////////
		{
			auto map00 = std::make_unique<GameObject>();
			XMStoreFloat4x4(&map00->World, XMMatrixScaling(1.0f, 1.0f, 1.0f)*XMMatrixTranslation(0.0f, -100.0f, 0.0f));
			XMStoreFloat4x4(&map00->TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
			map00->ObjCBIndex = objIndex++;
			map00->Mat = mMaterials["rand"].get();
			map00->Geo = mGeometries["map00Geo"].get();
			map00->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
			map00->bounds = mBounds["map00"];
			map00->IndexCount = map00->Geo->DrawArgs["map00"].IndexCount;
			map00->StartIndexLocation = map00->Geo->DrawArgs["map00"].StartIndexLocation;
			map00->BaseVertexLocation = map00->Geo->DrawArgs["map00"].BaseVertexLocation;

			mOpaqueRitems[(int)RenderLayer::Scene01_Map].push_back(map00.get());
			mAllRitems.push_back(std::move(map00));

			auto gridRitem = std::make_unique<GameObject>();
			XMStoreFloat4x4(&gridRitem->World, XMMatrixScaling(30.0f, 1.0f, 30.0f)*XMMatrixTranslation(0.0f, -250.0f, 0.0f));
			XMStoreFloat4x4(&gridRitem->TexTransform, XMMatrixScaling(5.0f, 5.0f, 1.0f));
			gridRitem->ObjCBIndex = objIndex++;
			gridRitem->Mat = mMaterials["rand"].get();
			gridRitem->Geo = mGeometries["shapeGeo"].get();
			gridRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
			gridRitem->bounds = mBounds["grid"];
			gridRitem->IndexCount = gridRitem->Geo->DrawArgs["grid"].IndexCount;
			gridRitem->StartIndexLocation = gridRitem->Geo->DrawArgs["grid"].StartIndexLocation;
			gridRitem->BaseVertexLocation = gridRitem->Geo->DrawArgs["grid"].BaseVertexLocation;

			BuildCollBoxGeometry(gridRitem->bounds, "gridBoxGeo", "gridBox", false);

			mOpaqueRitems[(int)RenderLayer::Scene01_Map].push_back(gridRitem.get());
			mAllRitems.push_back(std::move(gridRitem));

			auto map02 = std::make_unique<GameObject>();
			XMStoreFloat4x4(&map02->World, XMMatrixScaling(1.0f, 1.0f, 1.0f)*XMMatrixTranslation(0.0f, -100.0f, 0.0f));
			XMStoreFloat4x4(&map02->TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
			map02->ObjCBIndex = objIndex++;
			map02->Mat = mMaterials["bricks"].get();
			map02->Geo = mGeometries["map02Geo"].get();
			map02->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
			map02->bounds = mBounds["map02"];
			map02->IndexCount = map02->Geo->DrawArgs["map02"].IndexCount;
			map02->StartIndexLocation = map02->Geo->DrawArgs["map02"].StartIndexLocation;
			map02->BaseVertexLocation = map02->Geo->DrawArgs["map02"].BaseVertexLocation;
			mOpaqueRitems[(int)RenderLayer::Scene02_Map].push_back(map02.get());
			mAllRitems.push_back(std::move(map02));

			auto map02_00 = std::make_unique<GameObject>();
			XMStoreFloat4x4(&map02_00->World, XMMatrixScaling(1.0f, 1.0f, 1.0f)*XMMatrixTranslation(0.0f, -100.0f, 0.0f));
			XMStoreFloat4x4(&map02_00->TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
			map02_00->ObjCBIndex = objIndex++;
			map02_00->Mat = mMaterials["wood"].get();
			map02_00->Geo = mGeometries["map02_00Geo"].get();
			map02_00->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
			map02_00->bounds = mBounds["map02_00"];
			map02_00->IndexCount = map02_00->Geo->DrawArgs["map02_00"].IndexCount;
			map02_00->StartIndexLocation = map02_00->Geo->DrawArgs["map02_00"].StartIndexLocation;
			map02_00->BaseVertexLocation = map02_00->Geo->DrawArgs["map02_00"].BaseVertexLocation;
			mOpaqueRitems[(int)RenderLayer::Scene02_Map].push_back(map02_00.get());
			mAllRitems.push_back(std::move(map02_00));

		}
		///////////////////////////플레이어////////////////////////////////
		//BuildHelicopterGeometry(*m_gunShip.get());

		auto player = std::make_unique<GameObject>();
		XMStoreFloat4x4(&player->World, XMMatrixScaling(1.0f, 1.0f, 1.0f)*XMMatrixTranslation(0.0f, 300.0f, 0.0f));
		player->ObjCBIndex = objIndex++;
		player->Mat = mMaterials["robot"].get();
		player->Geo = mGeometries["robot_freeGeo"].get();
		player->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		player->IndexCount = player->Geo->DrawArgs["robot_free"].IndexCount;
		player->StartIndexLocation = player->Geo->DrawArgs["robot_free"].StartIndexLocation;
		player->BaseVertexLocation = player->Geo->DrawArgs["robot_free"].BaseVertexLocation;

		player->SkinnedCBIndex = 0;
		player->SkinnedModelInst = mSkinnedModelInst.get();

		//플레이어 충돌박스
		player->bounds.SetMaxMin(XMFLOAT3(50, 150, 50), XMFLOAT3(-50, 0, -50));
		BuildCollBoxGeometry(player->bounds, "robot_freeBoxGeo", "robot_freeBox", false);

		mOpaqueRitems[(int)RenderLayer::Player].push_back(player.get());
		mAllRitems.push_back(std::move(player));

		auto dummy = std::make_unique<GameObject>();
		XMStoreFloat4x4(&dummy->World, XMMatrixScaling(1.0f, 1.0f, 1.0f)*XMMatrixTranslation(0.0f, 300.0f, 0.0f));
		dummy->ObjCBIndex = objIndex++;
		dummy->Mat = mMaterials["robot"].get();
		dummy->Geo = mGeometries["robot_freeGeo"].get();
		dummy->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		dummy->IndexCount = dummy->Geo->DrawArgs["robot_free"].IndexCount;
		dummy->StartIndexLocation = dummy->Geo->DrawArgs["robot_free"].StartIndexLocation;
		dummy->BaseVertexLocation = dummy->Geo->DrawArgs["robot_free"].BaseVertexLocation;

		dummy->SkinnedCBIndex = 1;
		dummy->SkinnedModelInst = mSkinnedFriendModelInst.get();

		dummy->bounds.SetMaxMin(XMFLOAT3(50, 150, 50), XMFLOAT3(-50, 0, -50));
		BuildCollBoxGeometry(dummy->bounds, "dummy_freeBoxGeo", "dummy_freeBox", false);

		mOpaqueRitems[(int)RenderLayer::Friend].push_back(dummy.get());
		mAllRitems.push_back(std::move(dummy));

		//정적 오브젝트 ( 문,, 동상..)
		{
			for (int i = 0; i < 4; i++) {
				auto statue = std::make_unique<GameObject>();
				float myX = 0; float myZ = 0;
				if (i == 0) { myX = -600; myZ = -600; }
				else if (i == 1) { myX = -600; myZ = 600; }
				else if (i == 2) { myX = 700; myZ = -600; }
				else if (i == 3) { myX = 700; myZ = 600; }
				XMStoreFloat4x4(&statue->World, XMMatrixScaling(7, 7, 7) * XMMatrixRotationRollPitchYaw(1.55f, 0, 0) *XMMatrixTranslation(myX, 35.0f, myZ));
				statue->ObjCBIndex = objIndex++;
				statue->Mat = mMaterials["statue"].get();
				statue->Geo = mGeometries["statueGeo"].get();
				statue->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
				statue->IndexCount = statue->Geo->DrawArgs["statue"].IndexCount;
				statue->StartIndexLocation = statue->Geo->DrawArgs["statue"].StartIndexLocation;
				statue->BaseVertexLocation = statue->Geo->DrawArgs["statue"].BaseVertexLocation;

				mOpaqueRitems[(int)RenderLayer::Scene01_Map].push_back(statue.get());
				mAllRitems.push_back(std::move(statue));
			}
			auto door = std::make_unique<GameObject>();
			XMStoreFloat4x4(&door->World, XMMatrixScaling(1, 1, 1) *XMMatrixTranslation(600, 20.0f, 1900));
			door->ObjCBIndex = objIndex++;
			door->Mat = mMaterials["lever"].get();
			door->Geo = mGeometries["doorGeo"].get();
			door->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
			door->IndexCount = door->Geo->DrawArgs["door"].IndexCount;
			door->StartIndexLocation = door->Geo->DrawArgs["door"].StartIndexLocation;
			door->BaseVertexLocation = door->Geo->DrawArgs["door"].BaseVertexLocation;
			mOpaqueRitems[(int)RenderLayer::Scene01_Map].push_back(door.get());
			mAllRitems.push_back(std::move(door));

			auto doorFrame = std::make_unique<GameObject>();
			XMStoreFloat4x4(&doorFrame->World, XMMatrixScaling(1, 1, 1) *XMMatrixTranslation(600, 20.0f, 1900));
			doorFrame->ObjCBIndex = objIndex++;
			doorFrame->Mat = mMaterials["lever"].get();
			doorFrame->Geo = mGeometries["doorFrameGeo"].get();
			doorFrame->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
			doorFrame->IndexCount = doorFrame->Geo->DrawArgs["doorFrame"].IndexCount;
			doorFrame->StartIndexLocation = doorFrame->Geo->DrawArgs["doorFrame"].StartIndexLocation;
			doorFrame->BaseVertexLocation = doorFrame->Geo->DrawArgs["doorFrame"].BaseVertexLocation;
			mOpaqueRitems[(int)RenderLayer::Scene01_Map].push_back(doorFrame.get());
			mAllRitems.push_back(std::move(doorFrame));
		}

		//아이템
		auto items = std::make_unique<GameObject>();
		XMStoreFloat4x4(&items->World, XMMatrixScaling(0.7f, 0.7f, 0.7f)*XMMatrixTranslation(0.0f, 200.0f, 0.0f) * XMMatrixRotationRollPitchYaw(0, 0, -0.3));
		items->ObjCBIndex = objIndex++;
		items->Mat = mMaterials["handLight"].get();
		items->Geo = mGeometries["handLightGeo"].get();
		items->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		items->IndexCount = items->Geo->DrawArgs["handLight"].IndexCount;
		items->StartIndexLocation = items->Geo->DrawArgs["handLight"].StartIndexLocation;
		items->BaseVertexLocation = items->Geo->DrawArgs["handLight"].BaseVertexLocation;

		items->bounds.SetMaxMin(XMFLOAT3(100, 100, 100), XMFLOAT3(-100, -100, -100));
		BuildCollBoxGeometry(items->bounds, "handLightBoxGeo", "handLightBox", false);
		mOpaqueRitems[(int)RenderLayer::Item].push_back(items.get());
		mAllRitems.push_back(std::move(items));

		//장애물(창)
		for (int i = 0; i < 8; ++i) {
			auto spear = std::make_unique<GameObject>();
			XMStoreFloat4x4(&spear->World, XMMatrixScaling(5, 5, 10)*XMMatrixTranslation(0.0f + i*10, 200.0f, 0.0f) * XMMatrixRotationRollPitchYaw(-1.57, 0, 0));
			spear->ObjCBIndex = objIndex++;
			spear->Mat = mMaterials["lever"].get();
			spear->Geo = mGeometries["spearGeo"].get();
			spear->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
			spear->IndexCount = spear->Geo->DrawArgs["spear"].IndexCount;
			spear->StartIndexLocation = spear->Geo->DrawArgs["spear"].StartIndexLocation;
			spear->BaseVertexLocation = spear->Geo->DrawArgs["spear"].BaseVertexLocation;

			spear->bounds.SetMaxMin(XMFLOAT3(100, 400, 100), XMFLOAT3(-100, -400, -100));
			mOpaqueRitems[(int)RenderLayer::Spear].push_back(spear.get());
			mAllRitems.push_back(std::move(spear));
		}
		for (int i = 0; i < 8; ++i) {
			auto flareSpritesRitem = std::make_unique<GameObject>();
			flareSpritesRitem->World = MathHelper::Identity4x4();
			flareSpritesRitem->m_bActive = false;
			flareSpritesRitem->ObjCBIndex = objIndex++;
			flareSpritesRitem->Mat = mMaterials["flareSprites"].get();//treeSprites
			flareSpritesRitem->Geo = mGeometries["flareSpritesGeo"].get();
			flareSpritesRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
			flareSpritesRitem->IndexCount = flareSpritesRitem->Geo->DrawArgs["point"].IndexCount;
			flareSpritesRitem->StartIndexLocation = flareSpritesRitem->Geo->DrawArgs["point"].StartIndexLocation;
			flareSpritesRitem->BaseVertexLocation = flareSpritesRitem->Geo->DrawArgs["point"].BaseVertexLocation;

			mOpaqueRitems[(int)RenderLayer::Flare].push_back(flareSpritesRitem.get());
			mAllRitems.push_back(std::move(flareSpritesRitem));
		}


		for (int i = 0; i < 3; i++) {
			auto lever = std::make_unique<GameObject>();
			XMStoreFloat4x4(&lever->World, XMMatrixScaling(30, 30, 30)*XMMatrixTranslation(0.0f, 200.0f, 0.0f));
			lever->ObjCBIndex = objIndex++;
			lever->Mat = mMaterials["lever"].get();
			lever->Geo = mGeometries["leverGeo"].get();
			lever->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
			lever->IndexCount = lever->Geo->DrawArgs["lever"].IndexCount;
			lever->StartIndexLocation = lever->Geo->DrawArgs["lever"].StartIndexLocation;
			lever->BaseVertexLocation = lever->Geo->DrawArgs["lever"].BaseVertexLocation;

			lever->bounds.SetMaxMin(XMFLOAT3(100, 100, 100), XMFLOAT3(-100, -100, -100));
			mOpaqueRitems[(int)RenderLayer::Lever].push_back(lever.get());
			mAllRitems.push_back(std::move(lever));
		}

		//움직이는 타일
		for (int i = 0; i < 3; ++i) {
			auto tiles = std::make_unique<GameObject>();
			XMStoreFloat4x4(&tiles->World, XMMatrixScaling(1, 1, 1)*XMMatrixTranslation(0.0f, -250.0f, 300.0f));
			tiles->ObjCBIndex = objIndex++;
			tiles->Mat = mMaterials["rand"].get();
			tiles->Geo = mGeometries["tileGeo"].get();
			tiles->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
			tiles->bounds = mBounds["tile00"];
			tiles->IndexCount = tiles->Geo->DrawArgs["tile00"].IndexCount;
			tiles->StartIndexLocation = tiles->Geo->DrawArgs["tile00"].StartIndexLocation;
			tiles->BaseVertexLocation = tiles->Geo->DrawArgs["tile00"].BaseVertexLocation;
			BuildCollBoxGeometry(tiles->bounds, "tileBoxGeo", "tileBox", true);
			mOpaqueRitems[(int)RenderLayer::MoveTile].push_back(tiles.get());
			mAllRitems.push_back(std::move(tiles));
		}

		//정적 맵 충돌체
		{
			//튜토리얼
			for (int i = 0; i < 10; i++) {
				auto col00 = std::make_unique<GameObject>();
				XMStoreFloat4x4(&col00->World, XMMatrixScaling(1, 1, 1)*XMMatrixTranslation(0.0f, -100.0f, 0.0f));
				col00->ObjCBIndex = objIndex++;
				//모델좌표계상의 충돌체를 만들어준다.
				if (i == 0)col00->bounds.SetMaxMin(XMFLOAT3(150, 150, -150), XMFLOAT3(-150, -150, -1050));
				else if (i == 1)col00->bounds.SetMaxMin(XMFLOAT3(450, 150, -1050), XMFLOAT3(-450, -150, -1650));
				else if (i == 2)col00->bounds.SetMaxMin(XMFLOAT3(1050, 150, 150), XMFLOAT3(-1350, -150, -150));
				else if (i == 3)col00->bounds.SetMaxMin(XMFLOAT3(-1350, 150, 450), XMFLOAT3(-1650, -150, -750));
				else if (i == 4)col00->bounds.SetMaxMin(XMFLOAT3(1350, 150, 450), XMFLOAT3(1050, -150, -750));
				else if (i == 5)col00->bounds.SetMaxMin(XMFLOAT3(1650, 150, -450), XMFLOAT3(1350, -150, -750));
				else if (i == 6)col00->bounds.SetMaxMin(XMFLOAT3(150, 150, 1650), XMFLOAT3(-150, -150, 450));
				else if (i == 7)col00->bounds.SetMaxMin(XMFLOAT3(750, 150, 1650), XMFLOAT3(150, -150, 1350));
				else if (i == 8)col00->bounds.SetMaxMin(XMFLOAT3(-150, 150, 1650), XMFLOAT3(-450, -150, 1350));
				else if (i == 9)col00->bounds.SetMaxMin(XMFLOAT3(750, 150, 1950), XMFLOAT3(450, -150, 1650));
				mOpaqueRitems[(int)RenderLayer::MapCollision01].push_back(col00.get());
				mAllRitems.push_back(std::move(col00));
			}
			//맵2
			for (int i = 0; i < 16; i++) {
				auto col02 = std::make_unique<GameObject>();
				XMStoreFloat4x4(&col02->World, XMMatrixScaling(1, 1, 1)*XMMatrixTranslation(0.0f, -100.0f, 0.0f));
				col02->ObjCBIndex = objIndex++;
				//모델좌표계상의 충돌체를 만들어준다.
				if (i == 0)col02->bounds.SetMaxMin(XMFLOAT3(-2250 + 900, 150, 1950), XMFLOAT3(-2250, -150, 1950 - 900)); //스타트 3x3
				if (i == 1)col02->bounds.SetMaxMin(XMFLOAT3(-600 + 750, 150, 1416 + 150), XMFLOAT3(-600 - 750, -150, 1416 - 150)); //스타트 다리
				if (i == 2)col02->bounds.SetMaxMin(XMFLOAT3(-2100 + 150, 150, 900 + 150), XMFLOAT3(-2100 - 150, -150, -150)); //
				if (i == 3)col02->bounds.SetMaxMin(XMFLOAT3(-2400 + 150, 150, -450), XMFLOAT3(-2400 - 150, -150, -450 - 600)); //
				if (i == 4)col02->bounds.SetMaxMin(XMFLOAT3(-2100 + 150, 150, 900 + 150), XMFLOAT3(-2100 - 150, -150, -150)); //
				if (i == 5)col02->bounds.SetMaxMin(XMFLOAT3(-2100 - 150 + 1200, 150, -450), XMFLOAT3(-2100 - 150, -150, -750)); //
				if (i == 6)col02->bounds.SetMaxMin(XMFLOAT3(150, 150, -150), XMFLOAT3(-1050, -150, -1350)); //
				if (i == 7)col02->bounds.SetMaxMin(XMFLOAT3(150 + 1800, 150, -1050), XMFLOAT3(150, -150, -1350)); //
				if (i == 8)col02->bounds.SetMaxMin(XMFLOAT3(1950 + 900, 150, -750), XMFLOAT3(1950, -150, -750 - 900)); //
				if (i == 9)col02->bounds.SetMaxMin(XMFLOAT3(1950 + 600, 150, 150), XMFLOAT3(1950, -150, -450)); //
				if (i == 10)col02->bounds.SetMaxMin(XMFLOAT3(150 + 600, 150, 1500 + 450), XMFLOAT3(150, -150, 1500 - 450)); //위쪽 
				if (i == 11)col02->bounds.SetMaxMin(XMFLOAT3(150 + 300, 150, 1050), XMFLOAT3(150, -150, 750));
				if (i == 12)col02->bounds.SetMaxMin(XMFLOAT3(750 + 1200, 150, 1416 + 150), XMFLOAT3(750, -150, 1416 - 150));//사다리2
				if (i == 13)col02->bounds.SetMaxMin(XMFLOAT3(1950 + 900, 150, 1650), XMFLOAT3(1950, -150, 1650 - 900));//오
				if (i == 14)col02->bounds.SetMaxMin(XMFLOAT3(2250, 150, 750), XMFLOAT3(1950, -150, 450));
				if (i == 15)col02->bounds.SetMaxMin(XMFLOAT3(2250 + 900, 150, 1050), XMFLOAT3(1950 + 900, -150, 750));
				mOpaqueRitems[(int)RenderLayer::MapCollision02].push_back(col02.get());
				mAllRitems.push_back(std::move(col02));
			}
		}

		{
			//임시라인
			auto lines = std::make_unique<GameObject>();
			XMStoreFloat4x4(&lines->World, XMMatrixScaling(1.0f, 1.0f, 1.0f)*XMMatrixTranslation(0.0f, 0.0f, 0.0f));
			lines->ObjCBIndex = objIndex++;
			lines->Mat = mMaterials["robot"].get();
			lines->Geo = mGeometries["robot_freeBoxGeo"].get();
			lines->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
			lines->IndexCount = lines->Geo->DrawArgs["robot_freeBox"].IndexCount;
			lines->StartIndexLocation = lines->Geo->DrawArgs["robot_freeBox"].StartIndexLocation;
			lines->BaseVertexLocation = lines->Geo->DrawArgs["robot_freeBox"].BaseVertexLocation;
			mOpaqueRitems[(int)RenderLayer::CollBox].push_back(lines.get());
			mAllRitems.push_back(std::move(lines));

			auto line = std::make_unique<GameObject>();
			XMStoreFloat4x4(&line->World, XMMatrixScaling(1.0f, 1.0f, 1.0f)*XMMatrixTranslation(0.0f, 0.0f, 0.0f));
			line->ObjCBIndex = objIndex++;
			line->Mat = mMaterials["robot"].get();
			line->Geo = mGeometries["dummy_freeBoxGeo"].get();
			line->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
			line->IndexCount = line->Geo->DrawArgs["dummy_freeBox"].IndexCount;
			line->StartIndexLocation = line->Geo->DrawArgs["dummy_freeBox"].StartIndexLocation;
			line->BaseVertexLocation = line->Geo->DrawArgs["dummy_freeBox"].BaseVertexLocation;
			mOpaqueRitems[(int)RenderLayer::CollBox].push_back(line.get());
			mAllRitems.push_back(std::move(line));

			auto line2 = std::make_unique<GameObject>();
			XMStoreFloat4x4(&line2->World, XMMatrixScaling(1.0f, 1.0f, 1.0f)*XMMatrixTranslation(0.0f, 0.0f, 0.0f));
			line2->ObjCBIndex = objIndex++;
			line2->Mat = mMaterials["robot"].get();
			line2->Geo = mGeometries["handLightBoxGeo"].get();
			line2->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
			line2->IndexCount = line2->Geo->DrawArgs["handLightBox"].IndexCount;
			line2->StartIndexLocation = line2->Geo->DrawArgs["handLightBox"].StartIndexLocation;
			line2->BaseVertexLocation = line2->Geo->DrawArgs["handLightBox"].BaseVertexLocation;
			mOpaqueRitems[(int)RenderLayer::CollBox].push_back(line2.get());
			mAllRitems.push_back(std::move(line2));

			auto line3 = std::make_unique<GameObject>();
			XMStoreFloat4x4(&line3->World, XMMatrixScaling(1.0f, 1.0f, 1.0f)*XMMatrixTranslation(0.0f, 0.0f, 0.0f));
			line3->ObjCBIndex = objIndex++;
			line3->Mat = mMaterials["robot"].get();
			line3->Geo = mGeometries["tileBoxGeo"].get();
			line3->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
			line3->IndexCount = line3->Geo->DrawArgs["tileBox"].IndexCount;
			line3->StartIndexLocation = line3->Geo->DrawArgs["tileBox"].StartIndexLocation;
			line3->BaseVertexLocation = line3->Geo->DrawArgs["tileBox"].BaseVertexLocation;
			mOpaqueRitems[(int)RenderLayer::CollBox].push_back(line3.get());
			mAllRitems.push_back(std::move(line3));
		}
	}

	m_ObjIndex = objIndex;
}

void MyScene::DrawGameObjects(ID3D12GraphicsCommandList* cmdList, const std::vector<GameObject*>& ritems, const int itemState)
{
	UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
	UINT matCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(MaterialConstants));
	UINT skinnedCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(SkinnedConstants));

	auto objectCB = mCurrFrameResource->ObjectCB->Resource();
	auto matCB = mCurrFrameResource->MaterialCB->Resource();
	auto skinnedCB = mCurrFrameResource->SkinnedCB->Resource();

	// For each render item...
	for (size_t i = 0; i < ritems.size(); ++i)
	{
		auto ri = ritems[i];

		cmdList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
		cmdList->IASetIndexBuffer(&ri->Geo->IndexBufferView());
		cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

		CD3DX12_GPU_DESCRIPTOR_HANDLE tex(mCbvSrvUavDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
		tex.Offset(ri->Mat->DiffuseSrvHeapIndex, mCbvSrvUavDescriptorSize);

		D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex*objCBByteSize;
		D3D12_GPU_VIRTUAL_ADDRESS matCBAddress = matCB->GetGPUVirtualAddress() + ri->Mat->MatCBIndex*matCBByteSize;

		//스킨 인스턴스 ( 플레이어이기도함 )
		if (ri->SkinnedModelInst != nullptr)
		{
			CD3DX12_GPU_DESCRIPTOR_HANDLE tex2(mCbvSrvUavDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
			D3D12_GPU_VIRTUAL_ADDRESS skinnedCBAddress = skinnedCB->GetGPUVirtualAddress() + ri->SkinnedCBIndex*skinnedCBByteSize;
			tex2.Offset(ri->Mat->DiffuseSrvHeapIndex + 1, mCbvSrvUavDescriptorSize);
			cmdList->SetGraphicsRootDescriptorTable(0, tex);
			cmdList->SetGraphicsRootConstantBufferView(1, objCBAddress);
			cmdList->SetGraphicsRootConstantBufferView(3, matCBAddress);
			cmdList->SetGraphicsRootConstantBufferView(4, skinnedCBAddress);
			cmdList->SetGraphicsRootDescriptorTable(5, tex2);
		}
		else if ((int)RenderLayer::Scene01_Map == itemState || (int)RenderLayer::Scene02_Map == itemState || (int)RenderLayer::MoveTile == itemState) {
			CD3DX12_GPU_DESCRIPTOR_HANDLE tex2(mCbvSrvUavDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
			tex2.Offset(ri->Mat->DiffuseSrvHeapIndex + 1, mCbvSrvUavDescriptorSize);

			cmdList->SetGraphicsRootDescriptorTable(0, tex);
			cmdList->SetGraphicsRootConstantBufferView(1, objCBAddress);
			cmdList->SetGraphicsRootConstantBufferView(3, matCBAddress);
			cmdList->SetGraphicsRootDescriptorTable(5, tex2);
		}
		else {
			cmdList->SetGraphicsRootDescriptorTable(0, tex);
			cmdList->SetGraphicsRootConstantBufferView(1, objCBAddress);
			cmdList->SetGraphicsRootConstantBufferView(3, matCBAddress);
		}

		cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);

	}
}

void MyScene::DrawSceneToShadowMap()
{
	mCommandList->RSSetViewports(1, &mShadowMap->Viewport());
	mCommandList->RSSetScissorRects(1, &mShadowMap->ScissorRect());

	// Change to DEPTH_WRITE.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap->Resource(),
		D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE));

	UINT passCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants));

	// Clear the back buffer and depth buffer.
	mCommandList->ClearDepthStencilView(mShadowMap->Dsv(),
		D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

	// Set null render target because we are only going to draw to
	// depth buffer.  Setting a null render target will disable color writes.
	// Note the active PSO also must specify a render target count of 0.
	mCommandList->OMSetRenderTargets(0, nullptr, false, &mShadowMap->Dsv());

	// Bind the pass constant buffer for the shadow map pass.
	auto passCB = mCurrFrameResource->PassCB->Resource();
	D3D12_GPU_VIRTUAL_ADDRESS passCBAddress = passCB->GetGPUVirtualAddress() + 1 * passCBByteSize;
	mCommandList->SetGraphicsRootConstantBufferView(2, passCBAddress);

	mCommandList->SetPipelineState(mPSOs["shadow_opaque"].Get());
	DrawGameObjects(mCommandList.Get(), mOpaqueRitems[(int)RenderLayer::Item], (int)RenderLayer::Item);
	if (nowScene == 1)
		DrawGameObjects(mCommandList.Get(), mOpaqueRitems[(int)RenderLayer::Scene01_Map], (int)RenderLayer::Scene01_Map);
	else if (nowScene == 2) {
		DrawGameObjects(mCommandList.Get(), mOpaqueRitems[(int)RenderLayer::Scene02_Map], (int)RenderLayer::Scene02_Map);
		//DrawGameObjects(mCommandList.Get(), mOpaqueRitems[(int)RenderLayer::Spear], (int)RenderLayer::Spear);
	}
	DrawGameObjects(mCommandList.Get(), mOpaqueRitems[(int)RenderLayer::MoveTile], (int)RenderLayer::MoveTile);
	DrawGameObjects(mCommandList.Get(), mOpaqueRitems[(int)RenderLayer::Opaque], (int)RenderLayer::Opaque);

	mCommandList->SetPipelineState(mPSOs["shadow_skin_opaque"].Get());
	DrawGameObjects(mCommandList.Get(), mOpaqueRitems[(int)RenderLayer::Player], (int)RenderLayer::Player);
	DrawGameObjects(mCommandList.Get(), mOpaqueRitems[(int)RenderLayer::Friend], (int)RenderLayer::Friend);

	// Change back to GENERIC_READ so we can read the texture in a shader.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap->Resource(),
		D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_GENERIC_READ));
}

void MyScene::DrawFullscreenQuad(ID3D12GraphicsCommandList* cmdList)
{
	// Null-out IA stage since we build the vertex off the SV_VertexID in the shader.
	cmdList->IASetVertexBuffers(0, 1, nullptr);
	cmdList->IASetIndexBuffer(nullptr);
	cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	cmdList->DrawInstanced(6, 1, 0, 0);
}

std::array<const CD3DX12_STATIC_SAMPLER_DESC, 7> MyScene::GetStaticSamplers()
{
	// Applications usually only need a handful of samplers.  So just define them all up front
	// and keep them available as part of the root signature.  

	const CD3DX12_STATIC_SAMPLER_DESC pointWrap(
		0, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
		1, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
		2, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
		3, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap(
		4, // shaderRegister
		D3D12_FILTER_ANISOTROPIC, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressW
		0.0f,                             // mipLODBias
		8);                               // maxAnisotropy

	const CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp(
		5, // shaderRegister
		D3D12_FILTER_ANISOTROPIC, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressW
		0.0f,                              // mipLODBias
		8);                                // maxAnisotropy

	const CD3DX12_STATIC_SAMPLER_DESC shadow(
		6, // shaderRegister
		D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressW
		0.0f,                               // mipLODBias
		16,                                 // maxAnisotropy
		D3D12_COMPARISON_FUNC_LESS_EQUAL,
		D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK);

	return {
		pointWrap, pointClamp,
		linearWrap, linearClamp,
		anisotropicWrap, anisotropicClamp,
		shadow
	};
}