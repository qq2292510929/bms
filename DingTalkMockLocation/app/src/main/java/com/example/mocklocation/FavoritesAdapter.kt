package com.example.mocklocation

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.button.MaterialButton

class FavoritesAdapter(
    private var items: List<FavoriteLocation>,
    private val onUse: (FavoriteLocation) -> Unit,
    private val onDelete: (FavoriteLocation) -> Unit
) : RecyclerView.Adapter<FavoritesAdapter.ViewHolder>() {

    inner class ViewHolder(view: View) : RecyclerView.ViewHolder(view) {
        val tvName: TextView = view.findViewById(R.id.tvName)
        val tvCoord: TextView = view.findViewById(R.id.tvCoord)
        val btnUse: MaterialButton = view.findViewById(R.id.btnUse)
        val btnDelete: MaterialButton = view.findViewById(R.id.btnDelete)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ViewHolder {
        val view = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_favorite, parent, false)
        return ViewHolder(view)
    }

    override fun onBindViewHolder(holder: ViewHolder, position: Int) {
        val item = items[position]
        holder.tvName.text = item.name
        holder.tvCoord.text = String.format("%.6f, %.6f", item.latitude, item.longitude)
        holder.btnUse.setOnClickListener { onUse(item) }
        holder.btnDelete.setOnClickListener { onDelete(item) }
    }

    override fun getItemCount() = items.size

    fun updateList(newItems: List<FavoriteLocation>) {
        items = newItems
        notifyDataSetChanged()
    }
}
