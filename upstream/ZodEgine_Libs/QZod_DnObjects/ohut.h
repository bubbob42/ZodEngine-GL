#ifndef OHUT_H
#define OHUT_H

#include "zobject.h"
#include "Animals/ahutanimal.h"

class QZOD_DNOBJECTSSHARED_EXPORT OHut : public ZObject
{
	public:
		OHut(ZTime *ztime_, ZSettings *zsettings_ = nullptr, int palette_ = DESERT);
		~OHut();
		
		static void Init();

		void DoRender(ZMap &the_map, SDL_Surface *dest, int shift_x = 0, int shift_y = 0);
		void DoPreRender(ZMap &the_map, SDL_Surface *dest, int shift_x = 0, int shift_y = 0);
		bool NeedsPreRender() const { return true; }
		int Process();

		void SetOwner(team_type owner_);
		void SetMapImpassables(ZMap &tmap);
		void UnSetMapImpassables(ZMap &tmap);

		void ChangePalette(int palette_);

		bool IsDestroyableImpass() { return true; }
		bool CausesImpassAtCoord(int x, int y);
	private:
		static ZSDL_Surface render_img[MAX_PLANET_TYPES];

		void SetMaxHutAnimals();
		void CreateAnimals(int amount = 1);
		void SendAnimalsHome(int amount);
		bool GetExitToTile(int &tx, int &ty);

		int palette;

		vector<AHutAnimal*> hut_animals;
		double next_hut_animal_time;
		double next_max_hut_animal_time;
		int max_hut_animals;

		/* where the animals were at the end of the last Process (map
		 * pixels): DoPreRender skips them all when this box, widened by a
		 * sprite's half size, lies outside the view (ZOD_HUTCULL) */
		int ani_x0, ani_y0, ani_x1, ani_y1;
		bool ani_box;
};

#endif
