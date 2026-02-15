import pandas as pd
import folium
from folium.plugins import HeatMap
import os

# ===============================
# 1. 路径设置
# ===============================
input_path = "/Users/shanboqi/Desktop/未命名文件夹/20260202.csv"
output_html = "/Users/shanboqi/Desktop/未命名文件夹/mobile_heatmap_20260202.html"

# ===============================
# 2. 数据处理
# ===============================
df = pd.read_csv(input_path)
df["Time"] = pd.to_datetime(df["Time"])
start_time = df["Time"].min().strftime('%Y-%m-%d %H:%M:%S')

# 过滤异常坐标
df = df[(df["Lat"] != 0) & (df["Lng"] != 0)]
center_lat, center_lng = df["Lat"].mean(), df["Lng"].mean()

# ===============================
# 3. 创建 Folium 地图
# ===============================
# 创建基础地图
m = folium.Map(location=[center_lat, center_lng],
               zoom_start=17,
               tiles='OpenStreetMap') # 默认底图

# 添加高德/谷歌风格的卫星底图选项 (额外赠送，增强可视化效果)
folium.TileLayer(
    tiles='https://mt1.google.com/vt/lyrs=s&x={x}&y={y}&z={z}',
    attr='Google',
    name='Google Satellite',
    overlay=False,
    control=True
).add_to(m)

# --- PM2.5 热图层 ---
pm25_data = df[['Lat', 'Lng', 'PM25']].values.tolist()
pm25_layer = folium.FeatureGroup(name='PM2.5 Heatmap')
HeatMap(pm25_data, radius=15, blur=10, min_opacity=0.4).add_to(pm25_layer)
pm25_layer.add_to(m)

# --- VOC 热图层 (默认隐藏) ---
voc_data = df[['Lat', 'Lng', 'VOC']].values.tolist()
voc_layer = folium.FeatureGroup(name='VOC Heatmap', show=False)
HeatMap(voc_data, radius=15, blur=10, min_opacity=0.4,
        gradient={0.2: 'blue', 0.5: 'lime', 1: 'red'}).add_to(voc_layer)
voc_layer.add_to(m)

# --- 原始轨迹点 (用于精确定位) ---
marker_layer = folium.FeatureGroup(name='Individual Points', show=False)
for _, row in df.iterrows():
    folium.CircleMarker(
        location=[row['Lat'], row['Lng']],
        radius=2,
        color='white',
        weight=1,
        fill=True,
        fill_color='blue',
        popup=f"PM2.5: {row['PM25']} | VOC: {row['VOC']}"
    ).add_to(marker_layer)
marker_layer.add_to(m)

# ===============================
# 4. 图层控制 (关键修复点)
# ===============================
# 注意这里是 folium.LayerControl() 而不是从 plugins 导入
folium.LayerControl(collapsed=False).add_to(m)

# ===============================
# 5. 保存
# ===============================
m.save(output_html)
print(f"修复完成！交互式热图已保存至: {output_html}")