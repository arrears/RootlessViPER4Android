package me.timschneeberger.rootlessjamesdsp.utils

import android.annotation.SuppressLint
import android.content.Context
import android.content.SharedPreferences
import android.content.res.Resources
import android.media.AudioAttributes
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import androidx.annotation.StringRes
import androidx.core.content.getSystemService
import androidx.mediarouter.media.MediaControlIntent
import androidx.mediarouter.media.MediaRouteSelector
import androidx.mediarouter.media.MediaRouter
import me.timschneeberger.rootlessjamesdsp.R
import me.timschneeberger.rootlessjamesdsp.utils.RoutingObserver.DeviceGroup.Companion.usesSingleProfile
import me.timschneeberger.rootlessjamesdsp.utils.preferences.Preferences
import org.koin.core.component.KoinComponent
import org.koin.core.component.inject
import timber.log.Timber
import kotlin.properties.Delegates


class RoutingObserver(val context: Context) : MediaRouter.Callback(), KoinComponent,
    SharedPreferences.OnSharedPreferenceChangeListener {

    private val callbacks = mutableListOf<RoutingChangedCallback>()
    private val router by lazy { MediaRouter.getInstance(context) }
    private val prefs: Preferences.App by inject()

    var currentDevice by Delegates.observable<Device?>(null) { _, old, new ->
        if (old != new)
            callbacks.forEach { it.onRoutingDeviceChanged(new) }
    }

    private val AudioDeviceInfo.deviceGroup: DeviceGroup
        get() = DeviceGroup.from(type)

    private val audioManager by lazy { context.getSystemService<AudioManager>()!! }

    private val mediaAttributes by lazy {
        AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_MEDIA)
            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
            .build()
    }

    // MediaRouter only reports a route change when the user-visible route
    // changes. A device that connects and drops in the background (a watch
    // that briefly takes the Bluetooth link) can leave the selected route and
    // the real output disagreeing, and nothing calls back when it goes away.
    // Listening to the device list directly closes that gap.
    private val deviceCallback = object : AudioDeviceCallback() {
        override fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>?) = refresh()
        override fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>?) = refresh()
    }

    private val AudioDeviceInfo.asDevice: Device
        get() = Device(productName.toString(), address, deviceGroup)

    /**
     * The device media is actually playing on right now. Android 13 can answer
     * that directly; before that, the selected route is matched against the
     * connected outputs, which is the best older versions offer.
     */
    private fun resolveCurrentDevice(): Device? {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            try {
                audioManager.getAudioDevicesForAttributes(mediaAttributes)
                    .firstOrNull { it.isSink }
                    ?.let { return it.asDevice }
            }
            catch (ex: Exception) {
                Timber.w(ex, "getAudioDevicesForAttributes failed; falling back to the media route")
            }
        }
        return try {
            router.selectedRoute.findOutputDevice()
        }
        catch (ex: Exception) {
            Timber.w(ex, "Failed to resolve the selected media route")
            null
        }
    }

    /** Re-read the current output; listeners hear about it only if it changed. */
    fun refresh() {
        currentDevice = resolveCurrentDevice()
    }

    private fun MediaRouter.RouteInfo.findOutputDevice(): Device? {
        return context.getSystemService<AudioManager>()!!
            .getDevices(AudioManager.GET_DEVICES_OUTPUTS)
            .filter(AudioDeviceInfo::isSink)
            .find { info ->
                info.deviceGroup.let {
                    when {
                        isDeviceSpeaker -> it == DeviceGroup.SPEAKER
                        isBluetooth -> it == DeviceGroup.BLUETOOTH
                        isUsb -> it == DeviceGroup.USB
                        isHeadphone -> it == DeviceGroup.ANALOG
                        isHdmi -> it == DeviceGroup.HDMI
                        else -> it == DeviceGroup.OTHER
                    }
                }
            }?.let { device ->
                Device(
                    device.productName.toString(),
                    device.address,
                    device.deviceGroup
                )
            }
    }

    private val MediaRouter.RouteInfo.isHdmi: Boolean
        @SuppressLint("DiscouragedApi")
        /*
            Breaking changes in Android 14:
            - 'HDMI' was replaced with 'External Device'
            - resource 'default_audio_route_name_hdmi' was renamed to 'default_audio_route_name_external_device'
            https://cs.android.com/android/_/android/platform/frameworks/base/+/376bae463a9e46437047efbd082c7ac56e1ced3f
         */
        get() = try {
            Resources.getSystem().getText(
                Resources.getSystem().getIdentifier(
                    sdkAbove(34 /* FIXME Build.VERSION_CODES.UPSIDE_DOWN_CAKE */) {
                        "default_audio_route_name_external_device"
                    }.below {
                        "default_audio_route_name_hdmi"
                    },
                    "string", "android"
                )
            ).let { hdmiName ->
                router.defaultRoute == this && hdmiName == name
            }
        }
        catch (ex: Resources.NotFoundException) {
            Timber.e("HDMI route name string not found")
            Timber.i(ex)
            // Fallback to unsafe check
            router.defaultRoute == this && name.contains("HDMI")
        }


    private val MediaRouter.RouteInfo.isUsb: Boolean
        @SuppressLint("DiscouragedApi")
        get() = sdkAbove(Build.VERSION_CODES.Q) {
            Resources.getSystem().getText(
                Resources.getSystem().getIdentifier(
                    "default_audio_route_name_usb", "string", "android"
                )
            )
        }.below {
            ""
        }.let { usbName ->
            router.defaultRoute == this && usbName == name
        }

    private val MediaRouter.RouteInfo.isHeadphone: Boolean
        @SuppressLint("DiscouragedApi")
        get() = Resources.getSystem().getText(
            Resources.getSystem().getIdentifier(
                "default_audio_route_name_headphones", "string", "android"
            )
        ).let { hpName ->
            router.defaultRoute == this && hpName == name
        }

    override fun onSharedPreferenceChanged(sharedPreferences: SharedPreferences?, key: String?) {
        when(key) {
            context.getString(R.string.key_device_profiles_enable) -> {
                if(prefs.get<Boolean>(R.string.key_device_profiles_enable))
                    retrigger()
            }
        }
    }

    override fun onRouteSelected(router: MediaRouter, route: MediaRouter.RouteInfo, reason: Int) {
        refresh()
    }

    override fun onRouteChanged(router: MediaRouter, route: MediaRouter.RouteInfo) {
        refresh()
    }

    override fun onRouteRemoved(router: MediaRouter, route: MediaRouter.RouteInfo) {
        router.unselect(MediaRouter.UNSELECT_REASON_DISCONNECTED)
        // Never keep pointing at a device that is gone
        refresh()
    }

    fun retrigger() {
        callbacks.forEach { callback -> currentDevice?.let(callback::onRoutingDeviceChanged) }
    }

    fun registerOnRoutingChangeListener(callback: RoutingChangedCallback) {
        if (callbacks.isEmpty()) {
            prefs.registerOnSharedPreferenceChangeListener(this)
            router.addCallback(
                MediaRouteSelector.Builder()
                    .addControlCategory(MediaControlIntent.CATEGORY_LIVE_AUDIO)
                    .build(),
                this,
                0
            )
            audioManager.registerAudioDeviceCallback(deviceCallback, Handler(Looper.getMainLooper()))
            // Without this the current device stays unknown until the next
            // route event, which can be never if nothing is plugged or paired
            refresh()
        }
        callbacks.add(callback)
        currentDevice?.let(callback::onRoutingDeviceChanged)
    }

    fun unregisterOnRoutingChangeListener(callback: RoutingChangedCallback) {
        callbacks.remove(callback)

        if (callbacks.isEmpty()) {
            prefs.unregisterOnSharedPreferenceChangeListener(this)
            router.removeCallback(this)
            audioManager.unregisterAudioDeviceCallback(deviceCallback)
        }
    }

    interface RoutingChangedCallback {
        fun onRoutingDeviceChanged(device: Device?)
    }

    inner class Device(
        private val productName: String,
        private val address: String,
        val group: DeviceGroup,
    ) {
        init {
            // Log unexpected events
            if(!group.usesSingleProfile() && !hasProductName()) {
                Timber.e(IllegalStateException("Device has no valid product name: $this"))
            }
        }

        val id: String
            get() = if(group.usesSingleProfile())
                group.name.lowercase()
            else
                "${group.name.lowercase()}_${(if(group == DeviceGroup.BLUETOOTH) address else productName).lowercase().sanitize()}"
        val name: String
            // If single profile, only use generic name
            get() = if(group.usesSingleProfile())
                context.getString(group.nameRes)
            // If Bluetooth device without address, resort to single profile
            else if((group == DeviceGroup.BLUETOOTH && address.isBlank()))
                "${context.getString(group.nameRes)} (${context.getString(R.string.group_unknown)})"
            else {
                "${if(hasProductName()) productName else address} (${context.getString(group.nameRes)})".let {
                    // Cleanup USB driver name prefix
                    if(group == DeviceGroup.USB)
                        it.replace("USB-Audio - ", "")
                    else
                        it
                }
            }


        override fun toString() = "Device(id=$id, group=$group, name='$name', productName='$productName', address='$address')"
        // Equal when it is the same output, so re-reading it does not count as a change
        override fun equals(other: Any?) = other is Device && other.id == id && other.name == name
        override fun hashCode() = id.hashCode()
        private fun hasProductName(): Boolean {
            // Special case: you can connect two phones of the same model name via bluetooth
            return (productName != Build.MODEL || group == DeviceGroup.BLUETOOTH) && productName.isNotEmpty()
        }
        private fun String.sanitize() = this.replace("[ \\;/:*?\"<>|&']".toRegex(),"_")
    }

    enum class DeviceGroup(@StringRes val nameRes: Int) {
        /* WARNING: Names of existing enum members are used as stable ids and must not be changed */
        ANALOG(R.string.group_wired_headphones),
        BLUETOOTH(R.string.group_bluetooth),
        HDMI(R.string.group_hdmi),
        SPEAKER(R.string.group_speaker),
        USB(R.string.group_usb),
        OTHER(R.string.group_unknown);

        companion object {
            fun DeviceGroup.usesSingleProfile() = this == ANALOG || this == SPEAKER || this == OTHER

            fun from(type: Int) = when (type) {
                AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> SPEAKER
                AudioDeviceInfo.TYPE_WIRED_HEADSET,
                AudioDeviceInfo.TYPE_LINE_ANALOG,
                AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> ANALOG
                AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,
                AudioDeviceInfo.TYPE_BLE_HEADSET,
                AudioDeviceInfo.TYPE_BLE_SPEAKER -> BLUETOOTH
                AudioDeviceInfo.TYPE_USB_DEVICE,
                AudioDeviceInfo.TYPE_USB_ACCESSORY,
                AudioDeviceInfo.TYPE_USB_HEADSET -> USB
                AudioDeviceInfo.TYPE_HDMI_ARC,
                AudioDeviceInfo.TYPE_HDMI_EARC,
                AudioDeviceInfo.TYPE_HDMI -> HDMI
                else -> OTHER
            }
        }
    }
}