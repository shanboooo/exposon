import pandas as pd
import numpy as np
import folium
from folium.plugins import AntPath, Fullscreen
from branca.element import Template, MacroElement

# ===============================
# 1. 环境数据加载与强鲁棒性清洗
# ===============================
data_path = "/Volumes/NO NAME/20260213_data.csv"
output_env_html = "/Volumes/NO NAME/路面地形与环境特征分析.html"

# 定义列名：时间, 温度, 湿度, 气压, 地形描述, 纬度, 经度, 参考值/海拔
cols = ['time', 'temp', 'hum', 'pres', 'terrain', 'lat', 'lon', 'ref']
df_env = pd.read_csv(data_path, sep=',', header=None, names=cols)


def filter_gps_advanced(df, threshold=3.0):
    """
    高级离群点检测：剔除0值并利用MAD算法过滤经纬度漂移
    """
    mask = (df['lat'] > 1.0) & (df['lon'] > 1.0)  # 剔除零点
    df = df[mask].copy()

    for col in ['lat', 'lon']:
        median = df[col].median()
        # MAD = median(|Xi - median|)
        mad = (df[col] - median).abs().median()
        if mad == 0: continue
        # 0.6745 是正态分布下 MAD 与标准差的转换系数
        bound = threshold * (mad / 0.6745)
        df = df[(df[col] >= median - bound) & (df[col] <= median + bound)]
    return df


df_env_clean = filter_gps_advanced(df_env)

# ===============================
# 2. 地图初始化 (工业深色底图)
# ===============================
# 使用深色底图可以更好地衬托出温度和地形的色彩对比
m_env = folium.Map(location=[df_env_clean['lat'].mean(), df_env_clean['lon'].mean()],
                   zoom_start=15,
                   tiles='CartoDB dark_matter',
                   control_scale=True)

Fullscreen().add_to(m_env)

# ===============================
# 3. 坡度矢量与轨迹可视化
# ===============================
# 定义符合学术审美的地形配色方案 (地形动力学配色)
terrain_config = {
    'Uphill': {'color': '#EC7063', 'delay': 1200, 'weight': 6, 'label': '上坡激励段'},
    'Downhill': {'color': '#58D68D', 'delay': 600, 'weight': 6, 'label': '下坡重力段'},
    'Flat': {'color': '#5DADE2', 'delay': 2000, 'weight': 3, 'label': '水平基准段'}
}

# 按轨迹段绘制，以体现地形变换
for i in range(len(df_env_clean) - 1):
    row1 = df_env_clean.iloc[i]
    row2 = df_env_clean.iloc[i + 1]

    t_type = row1['terrain']
    conf = terrain_config.get(t_type, terrain_config['Flat'])

    # 使用 AntPath 模拟能量流向
    AntPath(
        locations=[[row1['lat'], row1['lon']], [row2['lat'], row2['lon']]],
        dash_array=[10, 20],
        delay=conf['delay'],
        color=conf['color'],
        pulse_color='#FFFFFF',
        weight=conf['weight'],
        tooltip=f"时间: {row1['time']} | 气压: {row1['pres']}hPa | 地形: {t_type}"
    ).add_to(m_env)

# ===============================
# 4. 环境参数测点 (采样节点标注)
# ===============================
# 每隔 3 个采样点打一个学术标注，避免视觉拥堵
for _, row in df_env_clean.iloc[::3].iterrows():
    folium.CircleMarker(
        location=[row['lat'], row['lon']],
        radius=2,
        color='#BDC3C7',
        fill=True,
        opacity=0.4,
        popup=folium.Popup(f"""
            <div style='width:150px; font-family: sans-serif; font-size:12px;'>
                <b style='color:#34495E;'>BMP280 环境监测</b><hr>
                <b>气压:</b> {row['pres']:.2f} hPa<br>
                <b>温度:</b> {row['temp']:.1f} °C<br>
                <b>相对湿度:</b> {row['hum']:.1f} %
            </div>""", max_width=200)
    ).add_to(m_env)

# ===============================
# 5. 专业地形图例 (右下角)
# ===============================
legend_env_html = """
{% macro html(this, kwargs) %}
<div style="
    position: fixed; bottom: 50px; right: 50px; width: 180px; height: 130px; 
    background-color: rgba(44, 62, 80, 0.8); border:1px solid #566573; z-index:9999; 
    font-size:12px; color: white; padding: 12px; border-radius: 8px;
    font-family: 'Helvetica Neue', Helvetica, Arial, sans-serif;
">
    <b style="font-size:13px; color: #FDFEFE;">地形动力学识别 (BMP280)</b><br>
    <div style="margin-top:10px;">
        <span style='color:#EC7063'>●</span> 爬坡区间 (Energy Loss)<br>
        <span style='color:#58D68D'>●</span> 下坡区间 (Kinetic Gain)<br>
        <span style='color:#5DADE2'>●</span> 平路区间 (Steady State)
    </div>
    <hr style="margin:8px 0; border:0.5px solid #566573;">
    <small style='color:#ABB2B9'基于气压梯度变化检测</small>
</div>
{% endmacro %}
"""
legend_env = MacroElement()
legend_env._template = Template(legend_env_html)
m_env.get_root().add_child(legend_env)

# ===============================
# 6. 保存与输出
# ===============================
m_env.save(output_env_html)
print(f"环境与地形可视化报告已生成: {output_env_html}")