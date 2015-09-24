#include <DB/Columns/ColumnString.h>
#include <DB/DataTypes/DataTypeString.h>
#include <DB/DataTypes/DataTypesNumberFixed.h>
#include <DB/DataTypes/DataTypeDateTime.h>
#include <DB/DataStreams/OneBlockInputStream.h>
#include <DB/Storages/StorageSystemReplicationQueue.h>
#include <DB/Storages/StorageReplicatedMergeTree.h>
#include <DB/Common/VirtualColumnUtils.h>


namespace DB
{


StorageSystemReplicationQueue::StorageSystemReplicationQueue(const std::string & name_)
	: name(name_)
	, columns{
		/// Свойства таблицы.
		{ "database", 				new DataTypeString	},
		{ "table", 					new DataTypeString	},
		{ "replica_name",			new DataTypeString	},
		/// Неизменяемые свойства элемента.
		{ "position", 				new DataTypeUInt32	},
		{ "node_name", 				new DataTypeString	},
		{ "type", 					new DataTypeString	},
		{ "create_time",			new DataTypeDateTime},
		{ "required_quorum", 		new DataTypeUInt32	},
		{ "source_replica", 		new DataTypeString	},
		{ "new_part_name", 			new DataTypeString	},
		{ "parts_to_merge", 		new DataTypeArray(new DataTypeString) },
		{ "is_detach",				new DataTypeUInt8	},
		{ "is_attach_unreplicated",	new DataTypeUInt8	},
		{ "attach_source_part_name",new DataTypeString	},
		/// Статус обработки элемента.
		{ "is_currently_executing",	new DataTypeUInt8	},
		{ "num_tries",				new DataTypeUInt32	},
		{ "last_exception",			new DataTypeString	},
		{ "last_attempt_time",		new DataTypeDateTime},
		{ "num_postponed",			new DataTypeUInt32	},
		{ "postpone_reason",		new DataTypeString	},
		{ "last_postpone_time",		new DataTypeDateTime},
	}
{
}

StoragePtr StorageSystemReplicationQueue::create(const std::string & name_)
{
	return (new StorageSystemReplicationQueue(name_))->thisPtr();
}


BlockInputStreams StorageSystemReplicationQueue::read(
	const Names & column_names,
	ASTPtr query,
	const Context & context,
	const Settings & settings,
	QueryProcessingStage::Enum & processed_stage,
	const size_t max_block_size,
	const unsigned threads)
{
	check(column_names);
	processed_stage = QueryProcessingStage::FetchColumns;

	/// Собираем набор реплицируемых таблиц.
	Databases replicated_tables;
	{
		Poco::ScopedLock<Poco::Mutex> lock(context.getMutex());

		for (const auto & db : context.getDatabases())
			for (const auto & table : db.second)
				if (typeid_cast<const StorageReplicatedMergeTree *>(table.second.get()))
					replicated_tables[db.first][table.first] = table.second;
	}

	ColumnWithTypeAndName col_database_to_filter		{ new ColumnString,	new DataTypeString,	"database" };
	ColumnWithTypeAndName col_table_to_filter			{ new ColumnString,	new DataTypeString,	"table" };

	for (auto & db : replicated_tables)
	{
		for (auto & table : db.second)
		{
			col_database_to_filter.column->insert(db.first);
			col_table_to_filter.column->insert(table.first);
		}
	}

	/// Определяем, какие нужны таблицы, по условиям в запросе.
	{
		Block filtered_block { col_database_to_filter, col_table_to_filter };

		VirtualColumnUtils::filterBlockWithQuery(query, filtered_block, context);

		if (!filtered_block.rows())
			return BlockInputStreams();

		col_database_to_filter 	= filtered_block.getByName("database");
		col_table_to_filter 	= filtered_block.getByName("table");
	}

	ColumnWithTypeAndName col_database					{ new ColumnString,	new DataTypeString,	"database" };
	ColumnWithTypeAndName col_table						{ new ColumnString,	new DataTypeString,	"table" };
	ColumnWithTypeAndName col_replica_name 				{ new ColumnString, 	new DataTypeString, "replica_name" };
	ColumnWithTypeAndName col_position 					{ new ColumnUInt32, 	new DataTypeUInt32, "position" };
	ColumnWithTypeAndName col_node_name 				{ new ColumnString, 	new DataTypeString, "node_name" };
	ColumnWithTypeAndName col_type 						{ new ColumnString, 	new DataTypeString, "type" };
	ColumnWithTypeAndName col_create_time 				{ new ColumnUInt32, 	new DataTypeDateTime, "create_time" };
	ColumnWithTypeAndName col_required_quorum 			{ new ColumnUInt32, 	new DataTypeUInt32, "required_quorum" };
	ColumnWithTypeAndName col_source_replica 			{ new ColumnString, 	new DataTypeString, "source_replica" };
	ColumnWithTypeAndName col_new_part_name 			{ new ColumnString, 	new DataTypeString, "new_part_name" };
	ColumnWithTypeAndName col_parts_to_merge 			{ new ColumnArray(new ColumnString), new DataTypeArray(new DataTypeString), "parts_to_merge" };
	ColumnWithTypeAndName col_is_detach 				{ new ColumnUInt8, 		new DataTypeUInt8, "is_detach" };
	ColumnWithTypeAndName col_is_attach_unreplicated 	{ new ColumnUInt8, 		new DataTypeUInt8, "is_attach_unreplicated" };
	ColumnWithTypeAndName col_attach_source_part_name 	{ new ColumnString, 	new DataTypeString, "attach_source_part_name" };
	ColumnWithTypeAndName col_is_currently_executing 	{ new ColumnUInt8, 		new DataTypeUInt8, "is_currently_executing" };
	ColumnWithTypeAndName col_num_tries 				{ new ColumnUInt32, 	new DataTypeUInt32, "num_tries" };
	ColumnWithTypeAndName col_last_exception 			{ new ColumnString, 	new DataTypeString, "last_exception" };
	ColumnWithTypeAndName col_last_attempt_time 		{ new ColumnUInt32, 	new DataTypeDateTime, "last_attempt_time" };
	ColumnWithTypeAndName col_num_postponed 			{ new ColumnUInt32, 	new DataTypeUInt32, "num_postponed" };
	ColumnWithTypeAndName col_postpone_reason 			{ new ColumnString, 	new DataTypeString, "postpone_reason" };
	ColumnWithTypeAndName col_last_postpone_time 		{ new ColumnUInt32, 	new DataTypeDateTime, "last_postpone_time" };

	StorageReplicatedMergeTree::LogEntriesData queue;
	String replica_name;

	for (size_t i = 0, tables_size = col_database_to_filter.column->size(); i < tables_size; ++i)
	{
		String database = (*col_database_to_filter.column)[i].safeGet<const String &>();
		String table = (*col_table_to_filter.column)[i].safeGet<const String &>();

		typeid_cast<StorageReplicatedMergeTree &>(*replicated_tables[database][table]).getQueue(queue, replica_name);

		for (size_t j = 0, queue_size = queue.size(); j < queue_size; ++j)
		{
			const auto & entry = queue[j];

			Array parts_to_merge;
			parts_to_merge.reserve(entry.parts_to_merge.size());
			for (const auto & name : entry.parts_to_merge)
				parts_to_merge.push_back(name);

			col_database				.column->insert(database);
			col_table					.column->insert(table);
			col_replica_name			.column->insert(replica_name);
			col_position				.column->insert(UInt64(j));
			col_node_name				.column->insert(entry.znode_name);
			col_type					.column->insert(entry.typeToString());
			col_create_time				.column->insert(UInt64(entry.create_time));
			col_required_quorum			.column->insert(UInt64(entry.quorum));
			col_source_replica			.column->insert(entry.source_replica);
			col_new_part_name			.column->insert(entry.new_part_name);
			col_parts_to_merge			.column->insert(parts_to_merge);
			col_is_detach				.column->insert(UInt64(entry.detach));
			col_is_attach_unreplicated	.column->insert(UInt64(entry.attach_unreplicated));
			col_attach_source_part_name	.column->insert(entry.source_part_name);
			col_is_currently_executing	.column->insert(UInt64(entry.currently_executing));
			col_num_tries				.column->insert(UInt64(entry.num_tries));
			col_last_exception			.column->insert(entry.exception ? entry.exception->displayText() : "");
			col_last_attempt_time		.column->insert(UInt64(entry.last_attempt_time));
			col_num_postponed			.column->insert(UInt64(entry.num_postponed));
			col_postpone_reason			.column->insert(entry.postpone_reason);
			col_last_postpone_time		.column->insert(UInt64(entry.last_postpone_time));
		}
	}

	Block block{
		col_database,
		col_table,
		col_replica_name,
		col_position,
		col_node_name,
		col_type,
		col_create_time,
		col_required_quorum,
		col_source_replica,
		col_new_part_name,
		col_parts_to_merge,
		col_is_detach,
		col_is_attach_unreplicated,
		col_attach_source_part_name,
		col_is_currently_executing,
		col_num_tries,
		col_last_exception,
		col_last_attempt_time,
		col_num_postponed,
		col_postpone_reason,
		col_last_postpone_time,
	};

	return BlockInputStreams(1, new OneBlockInputStream(block));
}


}