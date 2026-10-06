package com.example.mocklocation

import java.io.Serializable

/**
 * 收藏的位置信息
 */
data class FavoriteLocation(
    val id: Long = System.currentTimeMillis(),
    val name: String,
    val latitude: Double,
    val longitude: Double,
    val altitude: Double = 0.0,
    val address: String = ""
) : Serializable
