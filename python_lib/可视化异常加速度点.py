import pandas as pd
import numpy as np
import folium
from folium.plugins import HeatMap, AntPath, Fullscreen
from branca.element import Template, MacroElement

# ===============================
# 1. 数据处理与物理量转换
# ===============================
input_path = "/Volumes/NO NAME/20260213_shock.csv"
output_html = "/Volumes/NO NAME/路面异常特征分析报告.html"

df = pd.read_csv(input_path, sep=',', header=None,
                 names=['time', 'val1', 'val2', 'terrain', 'lat', 'lon'])

# 清洗无效经纬度
df = df[(df["lat"] != 0) & (df["lon"] != 0)].copy()

# 【学术化逻辑】计算合成矢量加速度幅值 (SVM - Signal Vector Magnitude)
# 公式: $SVM = \sqrt{a_x^2 + a_y^2}$，用于表征骑行过程中的非平稳振动强度
df['mag'] = np.sqrt(df['val1'] ** 2 + df['val2'] ** 2)


def filter_outliers_mad(df, threshold=3.5):
    """基于中位数绝对偏差(MAD)的鲁棒离群点检测"""
    for col in ['lat', 'lon']:
        median = df[col].median()
        mad = (df[col] - median).abs().median()
        if mad == 0: continue
        lower = median - threshold * (mad / 0.6745)
        upper = median + threshold * (mad / 0.6745)
        df = df[(df[col] >= lower) & (df[col] <= upper)]
    return df


df_clean = filter_outliers_mad(df)
df_sorted = df_clean.sort_values(by='mag', ascending=False)

# ===============================
# 2. 地图初始化 (高级审美品格底图)
# ===============================
center_lat, center_lon = df_clean['lat'].mean(), df_clean['lon'].mean()
m = folium.Map(location=[center_lat, center_lon],
               zoom_start=16,
               tiles='CartoDB positron',  # 这种灰白色底图最能衬托学术标注
               control_scale=True)

Fullscreen().add_to(m)

# ===============================
# 3. 核心可视化层
# ===============================

# A. 轨迹流向线 (AntPath) - 象征数据流与运动矢量
points = df_clean[['lat', 'lon']].values.tolist()
AntPath(points, delay=1500, weight=3, color="#5D6D7E", pulse_color="#FFFFFF", tooltip="采样路径轨迹").add_to(m)

# B. 分级响应图层 (按震动强度百分比分类)
# 采用更学术的“热色调”分级体系
levels = [
    (0.05, '#943126', '一级异常 (显著冲击 - Top 5%)'),
    (0.10, '#CB4335', '二级异常 (中度波动 - Top 10%)'),
    (0.20, '#F39C12', '三级异常 (轻微颠簸 - Top 20%)')
]

for p, color, label in levels:
    # 默认只展示 Top 10%，保持界面简洁
    fg = folium.FeatureGroup(name=label, show=(p == 0.10))
    top_n = df_sorted.head(int(len(df_sorted) * p))

    for _, row in top_n.iterrows():
        folium.CircleMarker(
            location=[row['lat'], row['lon']],
            radius=5,
            color=color,
            fill=True,
            fill_color=color,
            fill_opacity=0.7,
            # 弹窗内容学术化描述
            popup=folium.Popup(f"""
                <div style='width:180px; font-family: sans-serif;'>
                    <b>振动响应分析</b><br><hr>
                    <b>SVM幅值:</b> {row['mag']:.3f} g<br>
                    <b>采样时刻:</b> {row['time']}<br>
                    <b>传感器:</b> MPU6050
                </div>""", max_width=250)
        ).add_to(fg)
    fg.add_to(m)

# C. 统计热力图层 (隐喻局部路面劣化程度)
heat_data = df_sorted.head(int(len(df_sorted) * 0.15))[['lat', 'lon', 'mag']].values.tolist()
HeatMap(heat_data, name="异常加速度热力分布", radius=12, blur=8).add_to(m)

# ===============================
# 4. 专业学术图例嵌入 (左下角)
# ===============================
legend_html = """
{% macro html(this, kwargs) %}
<div style="
    position: fixed; bottom: 50px; left: 50px; width: 220px; height: 160px; 
    background-color: white; border:1px solid #CACFD2; z-index:9999; font-size:12px;
    padding: 10px; border-radius: 5px; box-shadow: 0 0 10px rgba(0,0,0,0.1);
    font-family: 'Helvetica Neue', Helvetica, Arial, sans-serif;
">
    <b style="font-size:13px;">MPU6050 骑行分析报告</b><br>
    <div style="margin-top:8px;">
        <span style='color:#943126'>●</span> 强力冲击响应 (Extreme)<br>
        <span style='color:#CB4335'>●</span> 显著震动区间 (High)<br>
        <span style='color:#F39C12'>●</span> 正常波动阈值 (Moderate)
    </div>
    <hr style="margin:8px 0;">
    <small>数据处理: 合成矢量加速度 (SVM)<br>

{% endmacro %}
"""
legend = MacroElement()
legend._template = Template(legend_html)
m.get_root().add_child(legend)

# ===============================
# 5. 图层控制与终结
# ===============================
folium.LayerControl(position='topright', collapsed=False).add_to(m)

m.save(output_html)
print(f"学术化地图分析已保存至: {output_html}")