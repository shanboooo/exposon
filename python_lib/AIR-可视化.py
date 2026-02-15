import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import cartopy.crs as ccrs
import cartopy.io.img_tiles as cimgt
from matplotlib import gridspec
from scipy.interpolate import interp1d
import warnings

# 忽略无意义警告
warnings.filterwarnings("ignore", category=RuntimeWarning)

# ===============================
# 路径设置
# ===============================
input_path = "/Users/shanboqi/Desktop/未命名文件夹/20260122.csv"
output_csv = "/Users/shanboqi/Desktop/未命名文件夹/processed_mobile_station_cn_time.csv"
output_png = "/Users/shanboqi/Desktop/未命名文件夹/mobile_station_static_optimized.png"

# ===============================
# 读数据 & 清洗
# ===============================
df = pd.read_csv(input_path)
df["time_utc"] = pd.to_datetime(df["Date_UTC"] + " " + df["Time_UTC"], utc=True)
df["time_cn"] = df["time_utc"] + pd.Timedelta(hours=8)

# 提取开始和结束时间用于展示
start_time = df["time_cn"].min().strftime('%Y-%m-%d %H:%M')
end_time = df["time_cn"].max().strftime('%H:%M')
time_range_str = f"Sampling Period: {start_time} - {end_time} (CST)"

df = df[(df["Lat"] != 0) & (df["Lon"] != 0)].copy()
df = df.sort_values("time_cn").reset_index(drop=True)
df.to_csv(output_csv, index=False)

# ===============================
# 沿时间插值
# ===============================
N = 600
t = np.arange(len(df))
t_new = np.linspace(t.min(), t.max(), N)


def interp_col(col):
    return interp1d(t, df[col].values, kind="linear")(t_new)


lon, lat = interp_col("Lon"), interp_col("Lat")
pm25, pm10 = interp_col("PM25"), interp_col("PM10")
voc, temp, humi = interp_col("VOC"), interp_col("Temp"), interp_col("Humi")

# ===============================
# 地图范围
# ===============================
pad = 0.003  # 稍微缩小边距使路径更饱满
lon_min, lon_max = lon.min() - pad, lon.max() + pad
lat_min, lat_max = lat.min() - pad, lat.max() + pad

vars_data = [
    ("PM2.5", pm25, "viridis"),
    ("PM10", pm10, "plasma"),
    ("VOC", voc, "cividis"),
    ("Temp (°C)", temp, "coolwarm"),
    ("Humidity (%)", humi, "YlGnBu"),
]

# ===============================
# 绘图开始
# ===============================
request = cimgt.GoogleTiles(style='street')
fig = plt.figure(figsize=(26, 9))  # 稍微收窄宽度

# 调整 wspace(子图水平间距) 减小空白
gs = gridspec.GridSpec(1, 5, figure=fig, wspace=0.15, left=0.02, right=0.98, bottom=0.15, top=0.82)

for i, (name, values, cmap) in enumerate(vars_data):
    ax = fig.add_subplot(gs[i], projection=request.crs)
    ax.set_extent([lon_min, lon_max, lat_min, lat_max], crs=ccrs.PlateCarree())

    # 添加底图
    ax.add_image(request, 15, interpolation='spline36')  # 缩放提高到15更清晰

    # 绘制散点
    sc = ax.scatter(
        lon, lat, c=values, cmap=cmap,
        s=40, alpha=0.85, edgecolors='white', linewidths=0.3,
        transform=ccrs.PlateCarree(), zorder=5
    )

    # 坐标轴标签 (只在最左侧显示维度，底部显示经度)
    gl = ax.gridlines(draw_labels=True, dms=False, x_inline=False, y_inline=False,
                      linewidth=0.5, color='gray', alpha=0.3, linestyle='--')
    gl.top_labels = False
    gl.right_labels = False
    if i > 0: gl.left_labels = False  # 关键：中间子图不显示Y轴标签

    gl.xlabel_style = {'size': 8}
    gl.ylabel_style = {'size': 8}

    # 色带：放在下方
    cbar = plt.colorbar(sc, ax=ax, orientation='horizontal', pad=0.08, shrink=0.85, aspect=25)
    cbar.ax.tick_params(labelsize=8)
    ax.set_title(name, fontsize=14, pad=10, fontweight='bold')

# ===============================
# 顶部文本信息 (标题 + 时间)
# ===============================
plt.suptitle("Mobile Environmental Sensing Analysis", fontsize=22, y=0.95, fontweight='bold')
# 添加时间范围副标题
fig.text(0.5, 0.89, time_range_str, ha='center', fontsize=14, color='#333333', fontweight='medium')

# ===============================
# 自动去除边框白边并保存
# ===============================
# bbox_inches="tight" 结合 pad_inches=0 会强制裁掉所有多余像素
plt.savefig(output_png, dpi=300, bbox_inches="tight", pad_inches=0.1)
plt.show()

print(f"Success! Map saved with time range: {time_range_str}")