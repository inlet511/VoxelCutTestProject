# 架构概览

## UGPUSDFCutter

**职责**: 
- 维护 CPU_SDFData
- 处理 GPU Compute Shader 切削
- 处理回读

**新增**: 
- 实现一个纯 C++ 接口，供外部快速查询 SDF。

---

## UHapticProbeComponent

**职责**: 
- 挂载在钻头 Actor 上
- 定义“点壳 (Point Shell)”形状
- 保存刚度参数
- 执行物理计算

**依赖**: 
- 持有一个指向 UGPUSDFCutter 的引用。

---

## 定义查询接口 (ISDFVolumeProvider)

为了解耦，我们定义一个轻量级的纯 C++ 接口。这样探针不需要知道切削的具体实现，只需要知道“这里有个场可以查”。


# 调用流程

## 1. 准备切削管理组件
- 准备一个用于切削的Actor，放在场景中，只需要挂一个GPUSDFCutter组件。
- 在GPUSDFCutter的属性中分配以下属性：
    - 切削对象(一个正方体Cube)
    - 切削工具(同样是一个正方体Cube)
    - 切削对象的VolumeTexture
    - 切削工具的VolumeTexture
    - 设置一个渲染SDF的材质实例(SDFMaterialInstance属性)

## 2. 准备受力探测器组件
- 准备一个用于探测受力情况的探针Actor,包含：
    - 一个有切削工具的外轮廓的StaticMeshComponent(不一定非要在本Actor上，也可以在下面的步骤上引用其他Actor的StaticMeshComponent)
    - 一个HapticProbeComponent
        - 设置 CutterActor 属性为第一步准备的切削管理Actor
        - 设置 Visual Mesh Ref 为切削工具的外轮廓组件 
        - 设置 Sampling Density, 在模型表面取样
        - 设置 Sampling Min Distance，防止随机点过密
        - 注意这个类的Debug属性栏中有几个Debug选项，目的是便于调试，正式使用的时候要记得关闭

- 计算受力：
    - 在Haptics相关的线程中调用
- 如果要换刀：
    1. 修改StaticMeshComponent的UStaticMesh
    2. 调用HpticProbComponent的UpdateProbeMesh()函数
